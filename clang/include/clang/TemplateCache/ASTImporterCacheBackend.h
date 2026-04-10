//===- ASTImporterCacheBackend.h - ASTImporter-based cache backend --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Concrete implementation of TemplateInstantiationCacheBackend using
// ASTImporter + minimal Preprocessor + ASTWriter/ASTReader.
//
// This lives in clangTemplateCache (not clangSerialization or clangFrontend)
// so it can freely link both without creating circular dependencies.
//
// Pattern: same as clangCrossTU — a bridging library that links both
// clangFrontend and clangSerialization simultaneously.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TEMPLATECACHE_ASTIMPORTERCACHEBACKEND_H
#define LLVM_CLANG_TEMPLATECACHE_ASTIMPORTERCACHEBACKEND_H

#include "clang/Serialization/TemplateInstantiationCache.h"
#include "clang/Serialization/PCHContainerOperations.h"

namespace clang {

class CodeGenOptions;
class ModuleCache;

/// Concrete cache backend that uses ASTImporter for serialization.
///
/// Write path: copies the specialization into a minimal temp ASTContext via
/// ASTImporter, then serializes using ASTWriter into a PCHBuffer.
///
/// Read path: loads the cached PCM via ASTReader (with validation disabled,
/// using MK_PrebuiltModule), then uses ASTImporter to copy the specialization
/// into the consuming TU's ASTContext.
class ASTImporterCacheBackend : public TemplateInstantiationCacheBackend {
public:
  ASTImporterCacheBackend(TemplateInstantiationCache &Cache,
                          ModuleCache &ModCache,
                          const PCHContainerReader &PCHReader,
                          const CodeGenOptions &CodeGenOpts);

  ~ASTImporterCacheBackend() override;

  bool tryLoad(Sema &S, ClassTemplateSpecializationDecl *D) override;
  void store(Sema &S, ClassTemplateSpecializationDecl *D) override;
  bool tryLoadFunc(Sema &S, FunctionDecl *D) override;
  void storeFunc(Sema &S, FunctionDecl *D) override;

private:
  /// Serialize a Decl into a PCM blob.
  bool serializeDecl(Sema &S, Decl *D, SmallVectorImpl<char> &Out);

  /// Deserialize a PCM blob and import the first matching decl into S.
  /// IsClass: true for ClassTemplateSpecializationDecl, false for FunctionDecl.
  bool deserializeAndImport(Sema &S, StringRef PCMBlob, bool IsClass,
                             Decl *TargetDecl);

  TemplateInstantiationCache &Cache;
  ModuleCache &ModCache;
  const PCHContainerReader &PCHReader;
  const CodeGenOptions &CodeGenOpts;
};

/// Create and register an ASTImporterCacheBackend with the given cache.
/// Called from ExecuteCompilerInvocation when the cache is enabled.
void registerASTImporterCacheBackend(TemplateInstantiationCache &Cache,
                                     ModuleCache &ModCache,
                                     const PCHContainerReader &PCHReader,
                                     const CodeGenOptions &CodeGenOpts);

/// Deserialize a PCM blob (produced by serializeDecl) and import the
/// specialization into the consuming TU's ASTContext.
///
/// Shared utility used by ASTImporterCacheBackend and DaemonCacheBackend
/// so the deserialization logic is not duplicated.
///
/// IsClass: true for ClassTemplateSpecializationDecl, false for FunctionDecl.
/// TargetDecl: the partially-constructed decl in the consuming TU.
/// Returns true if the import succeeded (instantiation should be skipped).
bool deserializeCacheBlob(Sema &S, StringRef PCMBlob, bool IsClass,
                          Decl *TargetDecl, ModuleCache &ModCache,
                          const PCHContainerReader &PCHReader,
                          const CodeGenOptions &CodeGenOpts);

} // namespace clang

#endif // LLVM_CLANG_TEMPLATECACHE_ASTIMPORTERCACHEBACKEND_H
