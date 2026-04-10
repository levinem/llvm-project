//===- CompositeCacheBackend.cpp - Composite cache backend ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/TemplateCache/CompositeCacheBackend.h"
#include "clang/TemplateCache/ASTImporterCacheBackend.h"
#include "clang/TemplateCache/DaemonCacheBackend.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Sema/Sema.h"

using namespace clang;

CompositeCacheBackend::CompositeCacheBackend(
    std::string DaemonURL, TemplateInstantiationCache &Cache,
    ModuleCache &ModCache, const PCHContainerReader &PCHReader,
    const CodeGenOptions &CodeGenOpts)
    : Daemon(std::make_unique<DaemonCacheBackend>(
          std::move(DaemonURL), Cache, ModCache, PCHReader, CodeGenOpts)),
      Local(std::make_unique<ASTImporterCacheBackend>(Cache, ModCache,
                                                      PCHReader, CodeGenOpts)) {
}

CompositeCacheBackend::~CompositeCacheBackend() = default;

bool CompositeCacheBackend::tryLoad(Sema &S,
                                     ClassTemplateSpecializationDecl *D) {
  // Check daemon first (fast network hit), then local disk.
  if (Daemon->tryLoad(S, D))
    return true;
  return Local->tryLoad(S, D);
}

void CompositeCacheBackend::store(Sema &S,
                                   ClassTemplateSpecializationDecl *D) {
  // Local writes only; daemon learns about new entries by scanning cache dirs.
  Local->store(S, D);
}

bool CompositeCacheBackend::tryLoadFunc(Sema &S, FunctionDecl *D) {
  if (Daemon->tryLoadFunc(S, D))
    return true;
  return Local->tryLoadFunc(S, D);
}

void CompositeCacheBackend::storeFunc(Sema &S, FunctionDecl *D) {
  Local->storeFunc(S, D);
}

//===----------------------------------------------------------------------===//
// Registration helper used by ExecuteCompilerInvocation
//===----------------------------------------------------------------------===//

void clang::registerCompositeCacheBackend(TemplateInstantiationCache &Cache,
                                           ModuleCache &ModCache,
                                           const PCHContainerReader &PCHReader,
                                           const CodeGenOptions &CodeGenOpts,
                                           StringRef DaemonURL) {
  if (!DaemonURL.empty()) {
    Cache.setBackend(std::make_unique<CompositeCacheBackend>(
        DaemonURL.str(), Cache, ModCache, PCHReader, CodeGenOpts));
  } else {
    Cache.setBackend(std::make_unique<ASTImporterCacheBackend>(
        Cache, ModCache, PCHReader, CodeGenOpts));
  }
}
