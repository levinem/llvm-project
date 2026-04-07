//===- TemplateInstantiationCache.cpp - Cross-TU template cache -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements cross-TU template instantiation caching using:
// - ASTImporter to copy specializations between ASTContexts
// - ASTWriter/ASTReader (via ASTUnit) for PCM serialization
// - Single index file with mmap for I/O-efficient lookups
//
// Index file format:
//   [Header]       Magic("CTIC", 4) + Version(4) + EntryCount(4)
//   [OffsetTable]  EntryCount × { SpecHash(64) + Offset(8) + Size(8) }
//   [DataSection]  Concatenated PCM blobs
//
//===----------------------------------------------------------------------===//

#include "clang/Serialization/TemplateInstantiationCache.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImporterSharedState.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/TemplateBase.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

// Index file constants.
static constexpr char IndexMagic[4] = {'C', 'T', 'I', 'C'};
static constexpr uint32_t IndexVersion = 1;
static constexpr size_t IndexHeaderSize = 12; // Magic(4) + Version(4) + Count(4)
static constexpr size_t HashLen = 64;         // BLAKE3 hex string length
// Hash(64) + Offset(8) + Size(8) + Flags(1) + BaseHash(64)
static constexpr size_t IndexEntrySize = HashLen + 8 + 8 + 1 + HashLen;

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

TemplateInstantiationCache::TemplateInstantiationCache(StringRef CachePath,
                                                       StringRef ContextHash,
                                                       FileManager &FM)
    : FileMgr(FM) {
  SmallString<256> Dir(CachePath);
  llvm::sys::path::append(Dir, ContextHash);
  CacheDir = std::string(Dir);

  if (auto EC = llvm::sys::fs::create_directories(CacheDir)) {
    Enabled = false;
    return;
  }

  Enabled = true;
}

TemplateInstantiationCache::~TemplateInstantiationCache() {
  if (Enabled && !PendingWrites.empty())
    flushPendingWrites();
}

//===----------------------------------------------------------------------===//
// Hash Computation
//===----------------------------------------------------------------------===//

std::string TemplateInstantiationCache::computeClassSpecHash(
    ClassTemplateSpecializationDecl *D, ASTContext &Ctx) {
  llvm::BLAKE3 Hasher;

  ClassTemplateDecl *TD = D->getSpecializedTemplate();
  Hasher.update(TD->getQualifiedNameAsString());

  const TemplateArgumentList &Args = D->getTemplateArgs();
  for (unsigned I = 0, N = Args.size(); I != N; ++I) {
    llvm::FoldingSetNodeID ID;
    Args[I].Profile(ID, Ctx);
    auto StableHash = ID.computeStableHash();
    Hasher.update(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(&StableHash), sizeof(StableHash)));
  }

  if (auto *Pattern = TD->getTemplatedDecl()) {
    SourceLocation Loc = Pattern->getLocation();
    if (Loc.isValid()) {
      FileID FID = Ctx.getSourceManager().getFileID(Loc);
      if (auto Content = Ctx.getSourceManager().getBufferDataOrNone(FID))
        Hasher.update(*Content);
    }
  }

  auto Result = Hasher.final();
  SmallString<64> HexHash;
  llvm::toHex(Result, /*LowerCase=*/true, HexHash);
  return std::string(HexHash);
}

std::string
TemplateInstantiationCache::computeFuncSpecHash(FunctionDecl *D,
                                                ASTContext &Ctx) {
  llvm::BLAKE3 Hasher;

  std::unique_ptr<MangleContext> MC(
      ItaniumMangleContext::create(Ctx, Ctx.getDiagnostics()));
  std::string MangledName;
  {
    llvm::raw_string_ostream OS(MangledName);
    if (MC->shouldMangleDeclName(D))
      MC->mangleName(D, OS);
    else
      OS << D->getNameAsString();
  }
  Hasher.update(MangledName);

  if (auto *TSI = D->getTemplateSpecializationInfo()) {
    const TemplateArgumentList *Args = TSI->TemplateArguments;
    for (unsigned I = 0, N = Args->size(); I != N; ++I) {
      llvm::FoldingSetNodeID ID;
      (*Args)[I].Profile(ID, Ctx);
      auto StableHash = ID.computeStableHash();
      Hasher.update(llvm::ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>(&StableHash),
          sizeof(StableHash)));
    }
  }

  if (FunctionTemplateDecl *FTD = D->getPrimaryTemplate()) {
    if (auto *Pattern = FTD->getTemplatedDecl()) {
      SourceLocation Loc = Pattern->getLocation();
      if (Loc.isValid()) {
        FileID FID = Ctx.getSourceManager().getFileID(Loc);
        if (auto Content = Ctx.getSourceManager().getBufferDataOrNone(FID))
          Hasher.update(*Content);
      }
    }
  }

  auto Result = Hasher.final();
  SmallString<64> HexHash;
  llvm::toHex(Result, /*LowerCase=*/true, HexHash);
  return std::string(HexHash);
}

//===----------------------------------------------------------------------===//
// Index File I/O
//===----------------------------------------------------------------------===//

std::string TemplateInstantiationCache::getIndexPath() const {
  SmallString<256> Path(CacheDir);
  llvm::sys::path::append(Path, "index.tic");
  return std::string(Path);
}

bool TemplateInstantiationCache::loadIndex() {
  if (IndexLoaded)
    return true;

  std::string Path = getIndexPath();
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path, /*IsText=*/false,
                                               /*RequiresNullTerminator=*/false,
                                               /*IsVolatile=*/false);
  if (!BufOrErr) {
    // No index file yet — not an error, just empty cache.
    IndexLoaded = true;
    return true;
  }

  IndexBuffer = std::move(*BufOrErr);
  StringRef Data = IndexBuffer->getBuffer();

  // Validate header.
  if (Data.size() < IndexHeaderSize)
    return false;

  if (Data.substr(0, 4) != StringRef(IndexMagic, 4))
    return false;

  uint32_t Version = llvm::support::endian::read32le(Data.data() + 4);
  if (Version != IndexVersion)
    return false;

  uint32_t Count = llvm::support::endian::read32le(Data.data() + 8);

  size_t TableSize = Count * IndexEntrySize;
  if (Data.size() < IndexHeaderSize + TableSize)
    return false;

  // Parse offset table.
  IndexEntries.reserve(Count);
  const char *TablePtr = Data.data() + IndexHeaderSize;

  for (uint32_t I = 0; I < Count; ++I) {
    IndexEntry Entry;
    memcpy(Entry.SpecHash, TablePtr, HashLen);
    TablePtr += HashLen;
    Entry.Offset = llvm::support::endian::read64le(TablePtr);
    TablePtr += 8;
    Entry.Size = llvm::support::endian::read64le(TablePtr);
    TablePtr += 8;
    Entry.Flags = static_cast<uint8_t>(*TablePtr);
    TablePtr += 1;
    memcpy(Entry.BaseHash, TablePtr, HashLen);
    TablePtr += HashLen;

    // Validate that the blob is within bounds (if not a failure marker).
    if (Entry.Flags != EF_SFINAEFailure) {
      size_t DataStart = IndexHeaderSize + TableSize;
      if (Entry.Offset + Entry.Size > Data.size() - DataStart)
        continue;
    }

    unsigned Idx = IndexEntries.size();
    IndexEntries.push_back(Entry);
    IndexMap[StringRef(IndexEntries.back().SpecHash, HashLen)] = Idx;
  }

  IndexLoaded = true;
  return true;
}

bool TemplateInstantiationCache::writeIndex() {
  // Merge existing entries with pending writes.
  // Build the complete index in memory, then atomic-write.

  // Collect all entries: existing + pending.
  struct FullEntry {
    char Hash[HashLen];
    StringRef Data;
    uint8_t Flags = EF_Success;
    char BaseHash[HashLen] = {};
  };
  std::vector<FullEntry> AllEntries;

  // Add existing entries (their data is in IndexBuffer).
  if (IndexBuffer) {
    size_t TableSize = IndexEntries.size() * IndexEntrySize;
    size_t DataStart = IndexHeaderSize + TableSize;
    StringRef BufData = IndexBuffer->getBuffer();

    for (const auto &E : IndexEntries) {
      FullEntry FE;
      memcpy(FE.Hash, E.SpecHash, HashLen);
      FE.Flags = E.Flags;
      memcpy(FE.BaseHash, E.BaseHash, HashLen);
      if (E.Flags != EF_SFINAEFailure && E.Size > 0)
        FE.Data = BufData.substr(DataStart + E.Offset, E.Size);
      AllEntries.push_back(FE);
    }
  }

  // Add pending entries.
  for (const auto &PE : PendingWrites) {
    FullEntry FE;
    assert(PE.SpecHash.size() == HashLen);
    memcpy(FE.Hash, PE.SpecHash.data(), HashLen);
    FE.Data = StringRef(PE.PCMData.data(), PE.PCMData.size());
    FE.Flags = PE.Flags;
    if (PE.BaseHash.size() == HashLen)
      memcpy(FE.BaseHash, PE.BaseHash.data(), HashLen);
    AllEntries.push_back(FE);
  }

  // Compute total size.
  uint32_t Count = AllEntries.size();
  size_t TableSize = Count * IndexEntrySize;
  size_t DataOffset = 0;
  size_t TotalDataSize = 0;
  for (const auto &E : AllEntries)
    TotalDataSize += E.Data.size();

  size_t TotalSize = IndexHeaderSize + TableSize + TotalDataSize;

  // Build the output buffer.
  SmallVector<char, 0> Output;
  Output.resize(TotalSize);
  char *Out = Output.data();

  // Header.
  memcpy(Out, IndexMagic, 4);
  llvm::support::endian::write32le(Out + 4, IndexVersion);
  llvm::support::endian::write32le(Out + 8, Count);
  Out += IndexHeaderSize;

  // Offset table — compute offsets as we go.
  uint64_t CurrentOffset = 0;
  char *TableOut = Out;
  for (const auto &E : AllEntries) {
    memcpy(TableOut, E.Hash, HashLen);
    TableOut += HashLen;
    llvm::support::endian::write64le(TableOut, CurrentOffset);
    TableOut += 8;
    llvm::support::endian::write64le(TableOut, E.Data.size());
    TableOut += 8;
    *TableOut = E.Flags;
    TableOut += 1;
    memcpy(TableOut, E.BaseHash, HashLen);
    TableOut += HashLen;
    CurrentOffset += E.Data.size();
  }

  // Data section.
  char *DataOut = Output.data() + IndexHeaderSize + TableSize;
  for (const auto &E : AllEntries) {
    memcpy(DataOut, E.Data.data(), E.Data.size());
    DataOut += E.Data.size();
  }

  // Atomic write the complete index file.
  std::string Path = getIndexPath();
  if (auto EC = atomicWrite(Path,
                             StringRef(Output.data(), Output.size()))) {
    ++CacheErrors;
    return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
// Serialization Helpers
//===----------------------------------------------------------------------===//

/// Create a minimal temporary ASTUnit for serialization.
static std::unique_ptr<ASTUnit>
createTemporaryASTUnit(const LangOptions &LangOpts,
                       const TargetInfo &Target) {
  std::vector<std::string> Args;
  Args.push_back("-xc++");
  if (LangOpts.CPlusPlus20)
    Args.push_back("-std=c++20");
  else if (LangOpts.CPlusPlus17)
    Args.push_back("-std=c++17");
  else if (LangOpts.CPlusPlus14)
    Args.push_back("-std=c++14");
  else if (LangOpts.CPlusPlus11)
    Args.push_back("-std=c++11");
  else
    Args.push_back("-std=c++03");

  Args.push_back("-target");
  Args.push_back(Target.getTriple().str());
  Args.push_back("-w");
  Args.push_back("-fsyntax-only");

  return tooling::buildASTFromCodeWithArgs("", Args, "template-cache.cc");
}

bool TemplateInstantiationCache::serializeToPCM(Sema &S, Decl *D,
                                                 SmallVectorImpl<char> &Out) {
  auto TempAST = createTemporaryASTUnit(
      S.getLangOpts(), S.getASTContext().getTargetInfo());
  if (!TempAST)
    return false;

  ASTContext &FromCtx = S.getASTContext();
  ASTContext &ToCtx = TempAST->getASTContext();
  FileManager &ToFM = TempAST->getFileManager();

  auto SharedState = std::make_shared<ASTImporterSharedState>(
      *ToCtx.getTranslationUnitDecl());
  ASTImporter Importer(ToCtx, ToFM, FromCtx,
                       S.getSourceManager().getFileManager(),
                       /*MinimalImport=*/false, SharedState);

  auto ResultOrErr = Importer.Import(D);
  if (!ResultOrErr) {
    llvm::consumeError(ResultOrErr.takeError());
    return false;
  }

  // Save to a temporary file, then read it back into the buffer.
  // ASTUnit::Save writes to a file path, not a buffer.
  SmallString<256> TmpPath;
  int TmpFD;
  if (auto EC = llvm::sys::fs::createTemporaryFile("tic", "pcm", TmpFD, TmpPath)) {
    return false;
  }
  ::close(TmpFD);

  if (TempAST->Save(std::string(TmpPath))) {
    llvm::sys::fs::remove(TmpPath);
    return false;
  }

  // Read the PCM file into the output buffer.
  auto BufOrErr = llvm::MemoryBuffer::getFile(TmpPath);
  llvm::sys::fs::remove(TmpPath);
  if (!BufOrErr)
    return false;

  StringRef PCMData = (*BufOrErr)->getBuffer();
  Out.assign(PCMData.begin(), PCMData.end());
  return true;
}

bool TemplateInstantiationCache::importFromPCM(Sema &S,
                                                llvm::MemoryBufferRef PCMData,
                                                bool IsClass) {
  // Write the PCM blob to a temporary file for ASTUnit::LoadFromASTFile.
  SmallString<256> TmpPath;
  int TmpFD;
  if (auto EC = llvm::sys::fs::createTemporaryFile("tic-load", "pcm",
                                                    TmpFD, TmpPath))
    return false;

  {
    llvm::raw_fd_ostream OS(TmpFD, /*shouldClose=*/true);
    OS.write(PCMData.getBufferStart(), PCMData.getBufferSize());
  }

  // Load the PCM.
  auto DiagOpts = std::make_shared<DiagnosticOptions>();
  IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(*llvm::vfs::getRealFileSystem(),
                                         *DiagOpts);

  auto PCHOps = std::make_shared<PCHContainerOperations>();
  FileSystemOptions FSOpts;
  HeaderSearchOptions HSOpts;

  auto CachedAST = ASTUnit::LoadFromASTFile(
      std::string(TmpPath), PCHOps->getRawReader(),
      ASTUnit::LoadEverything,
      llvm::vfs::getRealFileSystem(),
      DiagOpts, Diags, FSOpts, HSOpts,
      &S.getLangOpts(),
      /*OnlyLocalDecls=*/false,
      CaptureDiagsKind::None,
      /*AllowASTWithCompilerErrors=*/true,
      /*UserFilesAreVolatile=*/false);

  llvm::sys::fs::remove(TmpPath);

  if (!CachedAST)
    return false;

  // Find the target decl in the loaded AST.
  Decl *CachedDecl = nullptr;
  auto *TU = CachedAST->getASTContext().getTranslationUnitDecl();

  for (auto *TopDecl : TU->decls()) {
    // Check direct decls.
    if (IsClass) {
      if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(TopDecl)) {
        if (CTSD->isCompleteDefinition()) {
          CachedDecl = CTSD;
          break;
        }
      }
    } else {
      if (auto *FD = dyn_cast<FunctionDecl>(TopDecl)) {
        if (FD->hasBody()) {
          CachedDecl = FD;
          break;
        }
      }
    }

    // Check within namespaces.
    if (auto *NS = dyn_cast<NamespaceDecl>(TopDecl)) {
      for (auto *Inner : NS->decls()) {
        if (IsClass) {
          if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(Inner)) {
            if (CTSD->isCompleteDefinition()) {
              CachedDecl = CTSD;
              break;
            }
          }
        } else {
          if (auto *FD = dyn_cast<FunctionDecl>(Inner)) {
            if (FD->hasBody()) {
              CachedDecl = FD;
              break;
            }
          }
        }
      }
      if (CachedDecl) break;
    }
  }

  if (!CachedDecl)
    return false;

  // Import into the consuming TU.
  ASTContext &ToCtx = S.getASTContext();
  FileManager &ToFM = S.getSourceManager().getFileManager();

  auto SharedState = std::make_shared<ASTImporterSharedState>(
      *ToCtx.getTranslationUnitDecl());
  ASTImporter Importer(ToCtx, ToFM, CachedAST->getASTContext(),
                       CachedAST->getFileManager(),
                       /*MinimalImport=*/false, SharedState);

  auto ResultOrErr = Importer.Import(CachedDecl);
  if (!ResultOrErr) {
    llvm::consumeError(ResultOrErr.takeError());
    return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
// Atomic File Operations
//===----------------------------------------------------------------------===//

std::error_code
TemplateInstantiationCache::atomicWrite(StringRef Path, StringRef Contents) {
  SmallString<256> TempPath(Path);
  TempPath += "-%%%%%%%%";
  int FD;
  if (auto EC = llvm::sys::fs::createUniqueFile(TempPath, FD, TempPath))
    return EC;

  {
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS.write(Contents.data(), Contents.size());
    if (OS.has_error()) {
      llvm::sys::fs::remove(TempPath);
      return OS.error();
    }
  }

  return llvm::sys::fs::rename(TempPath, Path);
}

//===----------------------------------------------------------------------===//
// Cache Lookup (via Index)
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::MemoryBuffer>
TemplateInstantiationCache::lookupInIndex(StringRef SpecHash) {
  if (!loadIndex())
    return nullptr;

  auto It = IndexMap.find(SpecHash);
  if (It == IndexMap.end())
    return nullptr;

  const IndexEntry &Entry = IndexEntries[It->second];

  // SFINAE failure entries have no data.
  if (Entry.Flags == EF_SFINAEFailure)
    return nullptr;

  size_t TableSize = IndexEntries.size() * IndexEntrySize;
  size_t DataStart = IndexHeaderSize + TableSize;

  StringRef BufData = IndexBuffer->getBuffer();
  if (DataStart + Entry.Offset + Entry.Size > BufData.size())
    return nullptr;

  StringRef Blob = BufData.substr(DataStart + Entry.Offset, Entry.Size);

  // Step 12: Handle delta-compressed entries.
  if (Entry.Flags == EF_DeltaCompressed) {
    StringRef BaseHashRef(Entry.BaseHash, HashLen);
    auto BaseBuf = lookupInIndex(BaseHashRef);
    if (!BaseBuf)
      return nullptr; // Can't decompress without base.

    auto Decompressed = deltaDecompress(
        ArrayRef<char>(Blob.data(), Blob.size()),
        ArrayRef<char>(BaseBuf->getBufferStart(), BaseBuf->getBufferSize()));

    return llvm::MemoryBuffer::getMemBufferCopy(
        StringRef(Decompressed.data(), Decompressed.size()),
        "cached-spec-delta.pcm");
  }

  return llvm::MemoryBuffer::getMemBufferCopy(Blob, "cached-spec.pcm");
}

//===----------------------------------------------------------------------===//
// Step 9: Negative Caching (SFINAE Failures)
//===----------------------------------------------------------------------===//

bool TemplateInstantiationCache::isSFINAEFailureCached(StringRef Hash) {
  if (!Enabled || !loadIndex())
    return false;

  auto It = IndexMap.find(Hash);
  if (It == IndexMap.end())
    return false;

  const IndexEntry &Entry = IndexEntries[It->second];
  if (Entry.Flags == EF_SFINAEFailure) {
    ++SFINAECacheHits;
    return true;
  }
  return false;
}

void TemplateInstantiationCache::storeSFINAEFailure(StringRef Hash) {
  if (!Enabled)
    return;

  if (!loadIndex())
    return;
  if (IndexMap.count(Hash))
    return;
  if (KnownHashes.count(std::string(Hash)))
    return;

  PendingEntry PE;
  PE.SpecHash = std::string(Hash);
  PE.Flags = EF_SFINAEFailure;
  // No PCM data for failures — just the marker.

  KnownHashes.insert(std::string(Hash));
  PendingWrites.push_back(std::move(PE));
}

//===----------------------------------------------------------------------===//
// Step 10: Hierarchical Dependency Caching
//===----------------------------------------------------------------------===//

void TemplateInstantiationCache::recordDependency(StringRef SpecHash,
                                                   StringRef DepHash) {
  Dependencies[std::string(SpecHash)].push_back(std::string(DepHash));
}

SmallVector<std::string, 4>
TemplateInstantiationCache::getDependencies(StringRef SpecHash) {
  auto It = Dependencies.find(std::string(SpecHash));
  if (It != Dependencies.end())
    return It->second;
  return {};
}

//===----------------------------------------------------------------------===//
// Step 11: Pre-populated STL Cache
//===----------------------------------------------------------------------===//

bool TemplateInstantiationCache::loadSTLIndex() {
  if (STLIndexLoaded)
    return true;

  STLIndexLoaded = true;

  if (STLCachePath.empty())
    return false;

  SmallString<256> Path(STLCachePath);
  llvm::sys::path::append(Path, "index.tic");

  auto BufOrErr = llvm::MemoryBuffer::getFile(Path, /*IsText=*/false,
                                               /*RequiresNullTerminator=*/false);
  if (!BufOrErr)
    return false;

  STLIndexBuffer = std::move(*BufOrErr);
  StringRef Data = STLIndexBuffer->getBuffer();

  if (Data.size() < IndexHeaderSize)
    return false;
  if (Data.substr(0, 4) != StringRef(IndexMagic, 4))
    return false;

  uint32_t Version = llvm::support::endian::read32le(Data.data() + 4);
  if (Version != IndexVersion)
    return false;

  uint32_t Count = llvm::support::endian::read32le(Data.data() + 8);
  size_t TableSize = Count * IndexEntrySize;
  if (Data.size() < IndexHeaderSize + TableSize)
    return false;

  const char *TablePtr = Data.data() + IndexHeaderSize;
  STLIndexEntries.reserve(Count);

  for (uint32_t I = 0; I < Count; ++I) {
    IndexEntry Entry;
    memcpy(Entry.SpecHash, TablePtr, HashLen);
    TablePtr += HashLen;
    Entry.Offset = llvm::support::endian::read64le(TablePtr);
    TablePtr += 8;
    Entry.Size = llvm::support::endian::read64le(TablePtr);
    TablePtr += 8;
    Entry.Flags = EF_Success;
    memset(Entry.BaseHash, 0, HashLen);

    unsigned Idx = STLIndexEntries.size();
    STLIndexEntries.push_back(Entry);
    STLIndexMap[StringRef(STLIndexEntries.back().SpecHash, HashLen)] = Idx;
  }

  return true;
}

std::unique_ptr<llvm::MemoryBuffer>
TemplateInstantiationCache::lookupInSTLIndex(StringRef SpecHash) {
  if (!loadSTLIndex())
    return nullptr;

  auto It = STLIndexMap.find(SpecHash);
  if (It == STLIndexMap.end())
    return nullptr;

  const IndexEntry &Entry = STLIndexEntries[It->second];
  size_t TableSize = STLIndexEntries.size() * IndexEntrySize;
  size_t DataStart = IndexHeaderSize + TableSize;

  StringRef BufData = STLIndexBuffer->getBuffer();
  if (DataStart + Entry.Offset + Entry.Size > BufData.size())
    return nullptr;

  StringRef Blob = BufData.substr(DataStart + Entry.Offset, Entry.Size);
  return llvm::MemoryBuffer::getMemBufferCopy(Blob, "stl-cached-spec.pcm");
}

bool TemplateInstantiationCache::tryLoadFromSTLCache(Sema &S,
                                                      StringRef SpecHash,
                                                      bool IsClass) {
  auto PCMBuf = lookupInSTLIndex(SpecHash);
  if (!PCMBuf)
    return false;

  if (!importFromPCM(S, PCMBuf->getMemBufferRef(), IsClass))
    return false;

  ++STLCacheHits;
  return true;
}

//===----------------------------------------------------------------------===//
// Step 12: Delta Compression
//===----------------------------------------------------------------------===//

std::string TemplateInstantiationCache::getTemplateIdentity(Decl *D,
                                                             ASTContext &Ctx) {
  if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(D))
    return CTSD->getSpecializedTemplate()->getQualifiedNameAsString();
  if (auto *FD = dyn_cast<FunctionDecl>(D)) {
    if (auto *FTD = FD->getPrimaryTemplate())
      return FTD->getQualifiedNameAsString();
  }
  return "";
}

SmallVector<char, 0>
TemplateInstantiationCache::deltaCompress(ArrayRef<char> Data,
                                           ArrayRef<char> BaseData) {
  // Simple XOR-based delta compression.
  // For each byte, store the XOR with the base byte. Identical bytes
  // become 0x00, which compresses extremely well with LZ4.
  size_t MinLen = std::min(Data.size(), BaseData.size());
  SmallVector<char, 0> Delta;
  Delta.resize(Data.size());

  // XOR the common prefix.
  for (size_t I = 0; I < MinLen; ++I)
    Delta[I] = Data[I] ^ BaseData[I];

  // Copy any remaining bytes from Data (if Data is longer than Base).
  for (size_t I = MinLen; I < Data.size(); ++I)
    Delta[I] = Data[I];

  return Delta;
}

SmallVector<char, 0>
TemplateInstantiationCache::deltaDecompress(ArrayRef<char> DeltaData,
                                             ArrayRef<char> BaseData) {
  size_t MinLen = std::min(DeltaData.size(), BaseData.size());
  SmallVector<char, 0> Result;
  Result.resize(DeltaData.size());

  for (size_t I = 0; I < MinLen; ++I)
    Result[I] = DeltaData[I] ^ BaseData[I];

  for (size_t I = MinLen; I < DeltaData.size(); ++I)
    Result[I] = DeltaData[I];

  return Result;
}

//===----------------------------------------------------------------------===//
// Public API (Updated with Steps 9-12)
//===----------------------------------------------------------------------===//

bool TemplateInstantiationCache::tryLoadClassSpecialization(
    Sema &S, ClassTemplateSpecializationDecl *D) {
  if (!Enabled)
    return false;

  std::string Hash = computeClassSpecHash(D, S.getASTContext());

  // Step 9: Check for cached SFINAE failure.
  if (isSFINAEFailureCached(Hash)) {
    // This specialization previously failed SFINAE.
    // The caller will see this as a cache miss and proceed normally,
    // but we could potentially signal the failure directly.
    // For now, SFINAE failures are only cached for deduction contexts.
  }

  // Step 10: Import dependencies first for faster ASTImporter resolution.
  auto Deps = getDependencies(Hash);
  for (const auto &DepHash : Deps) {
    auto DepBuf = lookupInIndex(DepHash);
    if (DepBuf)
      importFromPCM(S, DepBuf->getMemBufferRef(), /*IsClass=*/true);
  }

  // Primary lookup.
  auto PCMBuf = lookupInIndex(Hash);

  // Step 11: Fall back to STL cache if user cache misses.
  if (!PCMBuf)
    PCMBuf = lookupInSTLIndex(Hash);

  if (!PCMBuf) {
    ++CacheMisses;
    return false;
  }

  if (!importFromPCM(S, PCMBuf->getMemBufferRef(), /*IsClass=*/true)) {
    ++CacheErrors;
    return false;
  }

  ++CacheHits;
  return true;
}

bool TemplateInstantiationCache::tryLoadFunctionSpecialization(
    Sema &S, FunctionDecl *D) {
  if (!Enabled)
    return false;

  std::string Hash = computeFuncSpecHash(D, S.getASTContext());

  // Step 10: Import dependencies first.
  auto Deps = getDependencies(Hash);
  for (const auto &DepHash : Deps) {
    auto DepBuf = lookupInIndex(DepHash);
    if (DepBuf)
      importFromPCM(S, DepBuf->getMemBufferRef(), /*IsClass=*/true);
  }

  auto PCMBuf = lookupInIndex(Hash);

  // Step 11: STL fallback.
  if (!PCMBuf)
    PCMBuf = lookupInSTLIndex(Hash);

  if (!PCMBuf) {
    ++CacheMisses;
    return false;
  }

  if (!importFromPCM(S, PCMBuf->getMemBufferRef(), /*IsClass=*/false)) {
    ++CacheErrors;
    return false;
  }

  ++CacheHits;
  return true;
}

void TemplateInstantiationCache::storeClassSpecialization(
    Sema &S, ClassTemplateSpecializationDecl *D) {
  if (!Enabled)
    return;

  if (!D->isCompleteDefinition() || D->isInvalidDecl())
    return;

  std::string Hash = computeClassSpecHash(D, S.getASTContext());

  if (!loadIndex())
    return;
  if (IndexMap.count(Hash))
    return;
  if (KnownHashes.count(Hash))
    return;

  PendingEntry PE;
  PE.SpecHash = Hash;
  if (!serializeToPCM(S, D, PE.PCMData)) {
    ++CacheErrors;
    return;
  }

  // Step 12: Try delta compression against a base specialization
  // from the same template.
  std::string TemplateId = getTemplateIdentity(D, S.getASTContext());
  if (!TemplateId.empty()) {
    auto BaseIt = DeltaBaseMap.find(TemplateId);
    if (BaseIt != DeltaBaseMap.end()) {
      // We have a base — try delta compression.
      auto BaseBuf = lookupInIndex(BaseIt->second);
      if (BaseBuf) {
        auto Delta = deltaCompress(
            ArrayRef<char>(PE.PCMData.data(), PE.PCMData.size()),
            ArrayRef<char>(BaseBuf->getBufferStart(),
                           BaseBuf->getBufferSize()));
        // Only use delta if it's significantly smaller.
        if (Delta.size() < PE.PCMData.size() * 3 / 4) {
          PE.PCMData = std::move(Delta);
          PE.Flags = EF_DeltaCompressed;
          PE.BaseHash = BaseIt->second;
          ++DeltaCompressions;
        }
      }
    } else {
      // First specialization for this template — it becomes the base.
      DeltaBaseMap[TemplateId] = Hash;
    }
  }

  KnownHashes.insert(Hash);
  PendingWrites.push_back(std::move(PE));
  ++CacheWrites;
}

void TemplateInstantiationCache::storeFunctionSpecialization(
    Sema &S, FunctionDecl *D) {
  if (!Enabled)
    return;

  if (D->isInvalidDecl() || !D->hasBody())
    return;

  std::string Hash = computeFuncSpecHash(D, S.getASTContext());

  if (!loadIndex())
    return;
  if (IndexMap.count(Hash))
    return;
  if (KnownHashes.count(Hash))
    return;

  PendingEntry PE;
  PE.SpecHash = Hash;
  if (!serializeToPCM(S, D, PE.PCMData)) {
    ++CacheErrors;
    return;
  }

  // Step 12: Delta compression for function specializations.
  std::string TemplateId = getTemplateIdentity(D, S.getASTContext());
  if (!TemplateId.empty()) {
    auto BaseIt = DeltaBaseMap.find(TemplateId);
    if (BaseIt != DeltaBaseMap.end()) {
      auto BaseBuf = lookupInIndex(BaseIt->second);
      if (BaseBuf) {
        auto Delta = deltaCompress(
            ArrayRef<char>(PE.PCMData.data(), PE.PCMData.size()),
            ArrayRef<char>(BaseBuf->getBufferStart(),
                           BaseBuf->getBufferSize()));
        if (Delta.size() < PE.PCMData.size() * 3 / 4) {
          PE.PCMData = std::move(Delta);
          PE.Flags = EF_DeltaCompressed;
          PE.BaseHash = BaseIt->second;
          ++DeltaCompressions;
        }
      }
    } else {
      DeltaBaseMap[TemplateId] = Hash;
    }
  }

  KnownHashes.insert(Hash);
  PendingWrites.push_back(std::move(PE));
  ++CacheWrites;
}

void TemplateInstantiationCache::flushPendingWrites() {
  if (PendingWrites.empty())
    return;

  if (!writeIndex()) {
    ++CacheErrors;
    return;
  }

  PendingWrites.clear();
}

//===----------------------------------------------------------------------===//
// Statistics (Updated with Steps 9-12)
//===----------------------------------------------------------------------===//

void TemplateInstantiationCache::printStats(raw_ostream &OS) const {
  if (!Enabled)
    return;

  unsigned Total = CacheHits + CacheMisses;
  double HitRate = Total > 0 ? (100.0 * CacheHits / Total) : 0.0;

  OS << "\n*** Template Instantiation Cache Statistics ***\n";
  OS << "  Cache directory:      " << CacheDir << "\n";
  OS << "  Index file:           " << getIndexPath() << "\n";
  OS << "  Index entries:        " << IndexEntries.size() << "\n";
  OS << "  Cache hits:           " << CacheHits << "\n";
  OS << "  Cache misses:         " << CacheMisses << "\n";
  OS << "  Cache writes:         " << CacheWrites << "\n";
  OS << "  Cache errors:         " << CacheErrors << "\n";
  OS << "  Hit rate:             " << llvm::format("%.1f%%", HitRate) << "\n";
  OS << "  SFINAE cache hits:    " << SFINAECacheHits << "\n";
  OS << "  STL cache hits:       " << STLCacheHits << "\n";
  OS << "  Delta compressions:   " << DeltaCompressions << "\n";
  OS << "\n";
}
