//===- CompositeCacheBackend.h - Composite cache backend ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CompositeCacheBackend chains a DaemonCacheBackend (remote, read-only) with
// an ASTImporterCacheBackend (local disk, read-write).
//
// On tryLoad():  try daemon first (fast network hit), then local disk.
// On store():    write to local disk only (daemon learns via directory scan).
//
// This is the backend registered when both -ftemplate-instantiation-cache=<dir>
// and -ftemplate-instantiation-cache-daemon=<url> are provided.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TEMPLATECACHE_COMPOSITECACHEBACKEND_H
#define LLVM_CLANG_TEMPLATECACHE_COMPOSITECACHEBACKEND_H

#include "clang/Serialization/TemplateInstantiationCache.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include <memory>
#include <string>

namespace clang {

class CodeGenOptions;
class ModuleCache;

/// Chains DaemonCacheBackend (read-only, remote) with ASTImporterCacheBackend
/// (read-write, local). tryLoad() checks the daemon first; store() writes
/// locally only.
class CompositeCacheBackend : public TemplateInstantiationCacheBackend {
public:
  CompositeCacheBackend(std::string DaemonURL,
                        TemplateInstantiationCache &Cache,
                        ModuleCache &ModCache,
                        const PCHContainerReader &PCHReader,
                        const CodeGenOptions &CodeGenOpts);

  ~CompositeCacheBackend() override;

  bool tryLoad(Sema &S, ClassTemplateSpecializationDecl *D) override;
  void store(Sema &S, ClassTemplateSpecializationDecl *D) override;
  bool tryLoadFunc(Sema &S, FunctionDecl *D) override;
  void storeFunc(Sema &S, FunctionDecl *D) override;

private:
  std::unique_ptr<TemplateInstantiationCacheBackend> Daemon;
  std::unique_ptr<TemplateInstantiationCacheBackend> Local;
};

/// Create and register a CompositeCacheBackend (daemon + local) or a plain
/// ASTImporterCacheBackend (local only), depending on whether DaemonURL is set.
void registerCompositeCacheBackend(TemplateInstantiationCache &Cache,
                                   ModuleCache &ModCache,
                                   const PCHContainerReader &PCHReader,
                                   const CodeGenOptions &CodeGenOpts,
                                   StringRef DaemonURL);

} // namespace clang

#endif // LLVM_CLANG_TEMPLATECACHE_COMPOSITECACHEBACKEND_H
