//===- ASTImporterCacheBackend.cpp - ASTImporter-based cache backend ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the ASTImporter-based serialization backend for the cross-TU
// template instantiation cache. Follows the patterns of clangCrossTU and
// ASTUnit::LoadFromASTFile.
//
// Write path: ASTImporter → minimal temp ASTContext → ASTWriter → PCHBuffer
// Read path:  InMemoryModuleCache → ASTReader(MK_PrebuiltModule, no validation)
//             → ASTImporter → consuming TU's ASTContext
//
//===----------------------------------------------------------------------===//

#include "clang/TemplateCache/ASTImporterCacheBackend.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImporterSharedState.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/InMemoryModuleCache.h"
#include "clang/Serialization/ModuleCache.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Serialization/TemplateInstantiationCache.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace clang;

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

ASTImporterCacheBackend::ASTImporterCacheBackend(
    TemplateInstantiationCache &Cache, ModuleCache &ModCache,
    const PCHContainerReader &PCHReader, const CodeGenOptions &CodeGenOpts)
    : Cache(Cache), ModCache(ModCache), PCHReader(PCHReader),
      CodeGenOpts(CodeGenOpts) {}

ASTImporterCacheBackend::~ASTImporterCacheBackend() = default;

//===----------------------------------------------------------------------===//
// Helpers: Minimal Preprocessor Construction
//
// Follows the ASTUnit::LoadFromASTFile pattern (ASTUnit.cpp:767-771):
// A minimal Preprocessor with IILookup=nullptr suffices for ASTWriter.
//===----------------------------------------------------------------------===//

/// Create a minimal Preprocessor backed by the given SourceManager.
/// Reuses LangOptions, TargetInfo, and FileManager from the current Sema.
/// Does NOT call InitializePreprocessor() — no macros, no file processing.
static std::shared_ptr<Preprocessor>
createMinimalPreprocessor(DiagnosticsEngine &Diags,
                          const LangOptions &LangOpts,
                          SourceManager &SrcMgr,
                          FileManager &FileMgr,
                          const TargetInfo *Target) {
  auto PPOpts = std::make_shared<PreprocessorOptions>();
  auto HSOpts = std::make_shared<HeaderSearchOptions>();
  auto HeaderInfo = std::make_unique<HeaderSearch>(*HSOpts, SrcMgr, Diags,
                                                   LangOpts, Target);
  // ModuleLoader stub — the temporary context has no modules.
  struct NullModuleLoader : public ModuleLoader {
    ModuleLoadResult loadModule(SourceLocation, ModuleIdPath,
                                Module::NameVisibilityKind,
                                bool) override { return ModuleLoadResult(); }
    void makeModuleVisible(Module *, Module::NameVisibilityKind,
                           SourceLocation) override {}
    GlobalModuleIndex *loadGlobalModuleIndex(SourceLocation) override {
      return nullptr;
    }
    bool lookupMissingImports(StringRef, SourceLocation) override {
      return false;
    }
  };
  static NullModuleLoader NullLoader;

  auto PP = std::make_shared<Preprocessor>(
      *PPOpts, Diags, const_cast<LangOptions &>(LangOpts), SrcMgr,
      *HeaderInfo, NullLoader,
      /*IILookup=*/nullptr,
      /*OwnsHeaderSearch=*/false);

  if (Target)
    PP->Initialize(*Target);

  return PP;
}

//===----------------------------------------------------------------------===//
// Write Path: Serialize a Decl to a PCM blob
//===----------------------------------------------------------------------===//

bool ASTImporterCacheBackend::serializeDecl(Sema &S, Decl *D,
                                             SmallVectorImpl<char> &Out) {
  ASTContext &FromCtx = S.getASTContext();
  DiagnosticsEngine &Diags = S.getDiagnostics();
  const LangOptions &LangOpts = S.getLangOpts();
  const TargetInfo &TargetInfo = FromCtx.getTargetInfo();
  FileManager &FM = S.getSourceManager().getFileManager();

  // 1. Create a minimal temporary ASTContext to hold just the specialization.
  auto TempSrcMgr = llvm::makeIntrusiveRefCnt<SourceManager>(Diags, FM);
  IdentifierTable TempIdents(LangOpts);
  SelectorTable TempSelectors;
  Builtin::Context TempBuiltins;
  auto TempCtx = llvm::makeIntrusiveRefCnt<ASTContext>(
      const_cast<LangOptions &>(LangOpts), *TempSrcMgr,
      TempIdents, TempSelectors, TempBuiltins, TU_Complete);
  TempCtx->InitBuiltinTypes(TargetInfo, /*AuxTarget=*/nullptr);

  // 2. Use ASTImporter to copy D into the temp context.
  auto SharedState = std::make_shared<ASTImporterSharedState>(
      *TempCtx->getTranslationUnitDecl());
  ASTImporter Importer(*TempCtx, FM, FromCtx, FM,
                       /*MinimalImport=*/false, SharedState);

  auto ResultOrErr = Importer.Import(D);
  if (!ResultOrErr) {
    llvm::consumeError(ResultOrErr.takeError());
    return false;
  }

  // 3. Create a minimal Preprocessor for ASTWriter.
  auto TempPP = createMinimalPreprocessor(Diags, LangOpts, *TempSrcMgr, FM,
                                           &TargetInfo);

  // 4. Serialize via ASTWriter into a PCHBuffer.
  auto Buffer = std::make_shared<PCHBuffer>();
  llvm::BitstreamWriter Stream(Buffer->Data);
  ASTWriter Writer(Stream, Buffer->Data, ModCache, CodeGenOpts,
                   /*Extensions=*/{},
                   /*IncludeTimestamps=*/false,
                   /*BuildingImplicitModule=*/false,
                   /*GeneratingReducedBMI=*/false);

  Writer.WriteAST(TempPP.get(), /*OutputFile=*/"", /*WritingModule=*/nullptr,
                  /*isysroot=*/"");

  if (Buffer->Data.empty())
    return false;

  Out.assign(Buffer->Data.begin(), Buffer->Data.end());
  return true;
}

//===----------------------------------------------------------------------===//
// Read Path: Deserialize a PCM blob and import into the consuming TU
//===----------------------------------------------------------------------===//

/// Free function shared between ASTImporterCacheBackend and DaemonCacheBackend.
/// Deserializes a PCM blob produced by serializeDecl() and imports the
/// matching specialization into the consuming TU's ASTContext via ASTReader.
bool clang::deserializeCacheBlob(Sema &S, StringRef PCMBlob, bool IsClass,
                                  Decl *TargetDecl, ModuleCache &ModCache,
                                  const PCHContainerReader &PCHReader,
                                  const CodeGenOptions &CodeGenOpts) {
  // 1. Register the blob as an in-memory PCM.
  // ASTReader checks InMemoryModuleCache before the filesystem.
  static std::atomic<unsigned> Counter{0};
  std::string VirtualPath =
      "tic://cached-spec-" + std::to_string(Counter++) + ".pcm";

  ModCache.getInMemoryModuleCache().addBuiltPCM(
      VirtualPath,
      llvm::MemoryBuffer::getMemBufferCopy(PCMBlob, VirtualPath));

  // 2. Create a minimal Preprocessor for ASTReader.
  // Reuse the consuming TU's SourceManager, Diagnostics, FileManager.
  // DisableValidationForModuleKind::All + MK_PrebuiltModule skip file checking.
  ASTContext &ToCtx = S.getASTContext();
  DiagnosticsEngine &Diags = S.getDiagnostics();
  const LangOptions &LangOpts = S.getLangOpts();
  SourceManager &SrcMgr = S.getSourceManager();

  auto TempPPOpts = std::make_shared<PreprocessorOptions>();
  auto TempHSOpts = std::make_shared<HeaderSearchOptions>();
  auto TempHeaderInfo = std::make_unique<HeaderSearch>(*TempHSOpts, SrcMgr,
                                                        Diags, LangOpts,
                                                        &ToCtx.getTargetInfo());
  struct NullModuleLoader : public ModuleLoader {
    ModuleLoadResult loadModule(SourceLocation, ModuleIdPath,
                                Module::NameVisibilityKind,
                                bool) override { return ModuleLoadResult(); }
    void makeModuleVisible(Module *, Module::NameVisibilityKind,
                           SourceLocation) override {}
    GlobalModuleIndex *loadGlobalModuleIndex(SourceLocation) override {
      return nullptr;
    }
    bool lookupMissingImports(StringRef, SourceLocation) override {
      return false;
    }
  };
  static NullModuleLoader NullLoader;

  auto LoadPP = std::make_shared<Preprocessor>(
      *TempPPOpts, Diags, const_cast<LangOptions &>(LangOpts), SrcMgr,
      *TempHeaderInfo, NullLoader,
      /*IILookup=*/nullptr,
      /*OwnsHeaderSearch=*/false);
  LoadPP->Initialize(ToCtx.getTargetInfo());

  // 3. Construct ASTReader with validation fully disabled.
  auto Reader = llvm::makeIntrusiveRefCnt<ASTReader>(
      *LoadPP, ModCache, &ToCtx, PCHReader,
      CodeGenOpts,
      /*Extensions=*/ArrayRef<std::shared_ptr<ModuleFileExtension>>(),
      /*isysroot=*/"",
      /*DisableValidationKind=*/DisableValidationForModuleKind::All,
      /*AllowASTWithCompilerErrors=*/true);

  // 4. Load the PCM from in-memory cache using MK_PrebuiltModule to bypass
  //    all input file validation (creates virtual file entries instead).
  ASTReader::ASTReadResult ReadResult = Reader->ReadAST(
      VirtualPath, serialization::MK_PrebuiltModule,
      SourceLocation(),
      ASTReader::ARR_Missing | ASTReader::ARR_OutOfDate |
          ASTReader::ARR_ConfigurationMismatch);

  if (ReadResult != ASTReader::Success)
    return false;

  // 5. Find the cached specialization in the loaded TU.
  // After ReadAST, the reader merges decls into ToCtx directly because we
  // passed &ToCtx to the ASTReader constructor.
  Decl *FoundDecl = nullptr;
  if (IsClass) {
    auto *Target = cast<ClassTemplateSpecializationDecl>(TargetDecl);
    ClassTemplateDecl *CTD = Target->getSpecializedTemplate();
    void *InsertPos = nullptr;
    FoundDecl = CTD->findSpecialization(
        Target->getTemplateArgs().asArray(), InsertPos);
  } else {
    auto *Target = cast<FunctionDecl>(TargetDecl);
    if (Target->hasBody())
      return true; // ASTReader merged it directly.
  }

  if (!FoundDecl)
    return false;

  if (IsClass) {
    auto *CTSD = cast<ClassTemplateSpecializationDecl>(FoundDecl);
    return CTSD->isCompleteDefinition();
  }

  return true;
}

bool ASTImporterCacheBackend::deserializeAndImport(Sema &S,
                                                    StringRef PCMBlob,
                                                    bool IsClass,
                                                    Decl *TargetDecl) {
  return deserializeCacheBlob(S, PCMBlob, IsClass, TargetDecl,
                               ModCache, PCHReader, CodeGenOpts);
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

bool ASTImporterCacheBackend::tryLoad(Sema &S,
                                       ClassTemplateSpecializationDecl *D) {
  std::string Hash = TemplateInstantiationCache::computeClassSpecHash(
      D, S.getASTContext());

  auto Blob = Cache.lookup(Hash);
  if (!Blob)
    Blob = Cache.lookupSTL(Hash);

  if (!Blob) {
    Cache.recordMiss();
    return false;
  }

  bool Ok = deserializeAndImport(S, Blob->getBuffer(), /*IsClass=*/true, D);
  if (Ok) {
    Cache.recordHit();
    return true;
  }

  Cache.recordError();
  return false;
}

void ASTImporterCacheBackend::store(Sema &S,
                                     ClassTemplateSpecializationDecl *D) {
  if (!D->isCompleteDefinition() || D->isInvalidDecl())
    return;

  std::string Hash = TemplateInstantiationCache::computeClassSpecHash(
      D, S.getASTContext());

  // Already cached by this or another TU.
  if (Cache.lookup(Hash))
    return;

  SmallVector<char, 0> Blob;
  if (!serializeDecl(S, D, Blob)) {
    Cache.recordError();
    return;
  }

  Cache.store(Hash, Blob);
}

bool ASTImporterCacheBackend::tryLoadFunc(Sema &S, FunctionDecl *D) {
  std::string Hash = TemplateInstantiationCache::computeFuncSpecHash(
      D, S.getASTContext());

  auto Blob = Cache.lookup(Hash);
  if (!Blob)
    Blob = Cache.lookupSTL(Hash);

  if (!Blob) {
    Cache.recordMiss();
    return false;
  }

  bool Ok = deserializeAndImport(S, Blob->getBuffer(), /*IsClass=*/false, D);
  if (Ok) {
    Cache.recordHit();
    return true;
  }

  Cache.recordError();
  return false;
}

void ASTImporterCacheBackend::storeFunc(Sema &S, FunctionDecl *D) {
  if (D->isInvalidDecl() || !D->hasBody())
    return;

  std::string Hash = TemplateInstantiationCache::computeFuncSpecHash(
      D, S.getASTContext());

  if (Cache.lookup(Hash))
    return;

  SmallVector<char, 0> Blob;
  if (!serializeDecl(S, D, Blob)) {
    Cache.recordError();
    return;
  }

  Cache.store(Hash, Blob);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void clang::registerASTImporterCacheBackend(TemplateInstantiationCache &Cache,
                                             ModuleCache &ModCache,
                                             const PCHContainerReader &PCHReader,
                                             const CodeGenOptions &CodeGenOpts) {
  Cache.setBackend(std::make_unique<ASTImporterCacheBackend>(
      Cache, ModCache, PCHReader, CodeGenOpts));
}
