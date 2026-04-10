//===- TemplateInstantiationCache.cpp - Cross-TU template cache -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Storage and index layer for cross-TU template instantiation caching.
// This file handles the on-disk index format, hash computation, delta
// compression, and cache lookup/store operations. It does NOT depend on
// clangFrontend or clangTooling — the Sema layer handles ASTImporter
// operations and passes raw PCM blobs to this class.
//
// Index file format:
//   [Header]       Magic("CTIC", 4) + Version(4) + EntryCount(4) = 12 bytes
//   [OffsetTable]  N × { Hash(64) + Offset(8) + Size(8) + Flags(1) + BaseHash(64) }
//   [DataSection]  Concatenated PCM blobs
//
//===----------------------------------------------------------------------===//

#include "clang/Serialization/TemplateInstantiationCache.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/TemplateBase.h"
#include "clang/Basic/FileManager.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

static constexpr char IndexMagic[4] = {'C', 'T', 'I', 'C'};
static constexpr uint32_t IndexVersion = 1;
static constexpr size_t IndexHeaderSize = 12;
static constexpr size_t HashLen = 64;
static constexpr size_t IndexEntrySize = HashLen + 8 + 8 + 1 + HashLen; // 145

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

  IndexLoaded = true;

  auto BufOrErr = llvm::MemoryBuffer::getFile(getIndexPath(), /*IsText=*/false,
                                               /*RequiresNullTerminator=*/false,
                                               /*IsVolatile=*/false);
  if (!BufOrErr)
    return true; // No index yet — empty cache, not an error.

  IndexBuffer = std::move(*BufOrErr);
  StringRef Data = IndexBuffer->getBuffer();

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

    if (Entry.Flags != EF_SFINAEFailure) {
      size_t DataStart = IndexHeaderSize + TableSize;
      if (Entry.Offset + Entry.Size > Data.size() - DataStart)
        continue;
    }

    unsigned Idx = IndexEntries.size();
    IndexEntries.push_back(Entry);
    IndexMap[StringRef(IndexEntries.back().SpecHash, HashLen)] = Idx;
  }

  return true;
}

bool TemplateInstantiationCache::writeIndex() {
  struct FullEntry {
    char Hash[HashLen];
    StringRef Data;
    uint8_t Flags = EF_Success;
    char BaseHash[HashLen] = {};
  };
  std::vector<FullEntry> AllEntries;

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

  uint32_t Count = AllEntries.size();
  size_t TableSize = Count * IndexEntrySize;
  size_t TotalDataSize = 0;
  for (const auto &E : AllEntries)
    TotalDataSize += E.Data.size();

  size_t TotalSize = IndexHeaderSize + TableSize + TotalDataSize;

  SmallVector<char, 0> Output;
  Output.resize(TotalSize);
  char *Out = Output.data();

  memcpy(Out, IndexMagic, 4);
  llvm::support::endian::write32le(Out + 4, IndexVersion);
  llvm::support::endian::write32le(Out + 8, Count);
  Out += IndexHeaderSize;

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

  char *DataOut = Output.data() + IndexHeaderSize + TableSize;
  for (const auto &E : AllEntries) {
    memcpy(DataOut, E.Data.data(), E.Data.size());
    DataOut += E.Data.size();
  }

  if (auto EC = atomicWrite(getIndexPath(),
                             StringRef(Output.data(), Output.size()))) {
    ++CacheErrors;
    return false;
  }
  return true;
}

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
// Delta Compression
//===----------------------------------------------------------------------===//

SmallVector<char, 0>
TemplateInstantiationCache::deltaCompress(ArrayRef<char> Data,
                                           ArrayRef<char> BaseData) {
  size_t MinLen = std::min(Data.size(), BaseData.size());
  SmallVector<char, 0> Delta;
  Delta.resize(Data.size());
  for (size_t I = 0; I < MinLen; ++I)
    Delta[I] = Data[I] ^ BaseData[I];
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
// Lookup
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::MemoryBuffer>
TemplateInstantiationCache::lookup(StringRef SpecHash) {
  if (!loadIndex())
    return nullptr;

  auto It = IndexMap.find(SpecHash);
  if (It == IndexMap.end())
    return nullptr;

  const IndexEntry &Entry = IndexEntries[It->second];
  if (Entry.Flags == EF_SFINAEFailure)
    return nullptr;

  size_t TableSize = IndexEntries.size() * IndexEntrySize;
  size_t DataStart = IndexHeaderSize + TableSize;

  StringRef BufData = IndexBuffer->getBuffer();
  if (DataStart + Entry.Offset + Entry.Size > BufData.size())
    return nullptr;

  StringRef Blob = BufData.substr(DataStart + Entry.Offset, Entry.Size);

  if (Entry.Flags == EF_DeltaCompressed) {
    StringRef BaseHashRef(Entry.BaseHash, HashLen);
    auto BaseBuf = lookup(BaseHashRef);
    if (!BaseBuf)
      return nullptr;
    auto Decompressed = deltaDecompress(
        ArrayRef<char>(Blob.data(), Blob.size()),
        ArrayRef<char>(BaseBuf->getBufferStart(), BaseBuf->getBufferSize()));
    return llvm::MemoryBuffer::getMemBufferCopy(
        StringRef(Decompressed.data(), Decompressed.size()),
        "cached-spec-delta.pcm");
  }

  return llvm::MemoryBuffer::getMemBufferCopy(Blob, "cached-spec.pcm");
}

bool TemplateInstantiationCache::isSFINAEFailureCached(StringRef SpecHash) {
  if (!loadIndex())
    return false;
  auto It = IndexMap.find(SpecHash);
  if (It == IndexMap.end())
    return false;
  return IndexEntries[It->second].Flags == EF_SFINAEFailure;
}

//===----------------------------------------------------------------------===//
// Store
//===----------------------------------------------------------------------===//

void TemplateInstantiationCache::store(StringRef SpecHash,
                                        ArrayRef<char> PCMData) {
  if (!Enabled || !loadIndex())
    return;
  if (IndexMap.count(SpecHash))
    return;
  std::string HashStr(SpecHash);
  if (KnownHashes.count(HashStr))
    return;

  PendingEntry PE;
  PE.SpecHash = HashStr;
  PE.PCMData.assign(PCMData.begin(), PCMData.end());

  KnownHashes.insert(HashStr);
  PendingWrites.push_back(std::move(PE));
  ++CacheWrites;
}

void TemplateInstantiationCache::storeSFINAEFailure(StringRef SpecHash) {
  if (!Enabled || !loadIndex())
    return;
  if (IndexMap.count(SpecHash))
    return;
  std::string HashStr(SpecHash);
  if (KnownHashes.count(HashStr))
    return;

  PendingEntry PE;
  PE.SpecHash = HashStr;
  PE.Flags = EF_SFINAEFailure;

  KnownHashes.insert(HashStr);
  PendingWrites.push_back(std::move(PE));
}

void TemplateInstantiationCache::flushPendingWrites() {
  if (PendingWrites.empty())
    return;
  if (!writeIndex())
    ++CacheErrors;
  PendingWrites.clear();
}

//===----------------------------------------------------------------------===//
// Dependencies
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
// STL Cache
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

  if (Data.size() < IndexHeaderSize ||
      Data.substr(0, 4) != StringRef(IndexMagic, 4))
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
    Entry.Flags = static_cast<uint8_t>(*TablePtr);
    TablePtr += 1;
    memcpy(Entry.BaseHash, TablePtr, HashLen);
    TablePtr += HashLen;

    unsigned Idx = STLIndexEntries.size();
    STLIndexEntries.push_back(Entry);
    STLIndexMap[StringRef(STLIndexEntries.back().SpecHash, HashLen)] = Idx;
  }
  return true;
}

std::unique_ptr<llvm::MemoryBuffer>
TemplateInstantiationCache::lookupSTL(StringRef SpecHash) {
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

//===----------------------------------------------------------------------===//
// Statistics
//===----------------------------------------------------------------------===//

void TemplateInstantiationCache::printStats(raw_ostream &OS) const {
  if (!Enabled)
    return;

  unsigned Total = CacheHits + CacheMisses;
  double HitRate = Total > 0 ? (100.0 * CacheHits / Total) : 0.0;
  unsigned LocalHits = CacheHits - DaemonHits;

  // Estimated time saved: each cache hit skips ~3ms of instantiation work
  // (conservative estimate for a single class specialization).
  // Daemon hits also save ~2ms of local disk I/O on top of that.
  static constexpr double kInstantiationMs = 3.0;
  static constexpr double kDiskIOMs = 2.0;
  double EstimatedSavedMs =
      CacheHits * kInstantiationMs + DaemonHits * kDiskIOMs;

  OS << "\n*** Template Instantiation Cache Statistics ***\n";
  OS << "  Cache directory:      " << CacheDir << "\n";
  OS << "  Index file:           " << getIndexPath() << "\n";
  OS << "  Index entries:        " << IndexEntries.size() << "\n";

  OS << "\n  --- Hits ---\n";
  OS << "  Total hits:           " << CacheHits << "\n";
  if (DaemonHits > 0) {
    OS << "    Daemon hits:        " << DaemonHits << "\n";
    OS << "    Local disk hits:    " << LocalHits << "\n";
  }
  OS << "  SFINAE cache hits:    " << SFINAECacheHits << "\n";
  OS << "  STL cache hits:       " << STLCacheHits << "\n";

  OS << "\n  --- Misses / Writes ---\n";
  OS << "  Cache misses:         " << CacheMisses << "\n";
  OS << "  Cache writes:         " << CacheWrites << "\n";
  OS << "  Cache errors:         " << CacheErrors << "\n";
  OS << "  Delta compressions:   " << DeltaCompressions << "\n";

  OS << "\n  --- Performance ---\n";
  OS << "  Hit rate:             " << llvm::format("%.1f%%", HitRate) << "\n";
  OS << "  Est. time saved:      "
     << llvm::format("%.0fms", EstimatedSavedMs)
     << " (~" << kInstantiationMs << "ms/hit, "
     << kDiskIOMs << "ms extra per daemon hit)\n";
  if (DaemonHits > 0) {
    double DaemonFraction = 100.0 * DaemonHits / CacheHits;
    OS << "  Daemon share:         "
       << llvm::format("%.1f%%", DaemonFraction)
       << " of hits served remotely\n";
  }
  OS << "\n";
}
