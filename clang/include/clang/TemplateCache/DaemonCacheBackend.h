//===- DaemonCacheBackend.h - HTTP daemon cache backend ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// HTTP GET-based cache backend that fetches PCM blobs from a running
// clang-template-cache-daemon. The daemon aggregates local caches from
// distributed build workers and serves them read-only over HTTP.
//
// This backend is OPTIONAL: it is only used when the compiler is invoked with
// -ftemplate-instantiation-cache-daemon=<url>. Without that flag, the local
// ASTImporterCacheBackend is used exclusively.
//
// Architecture:
//   tryLoad()   — HTTP GET <daemon_url>/pcm/<hash>
//                 On 200: deserialize blob into consuming TU via deserializeCacheBlob()
//                 On 404/error: return false (caller falls back to local cache)
//   store()     — no-op (daemon is read-only from the compiler's perspective;
//                 local writes go through ASTImporterCacheBackend)
//
// The daemon itself is populated by scanning build workers' local index.tic
// files, following the same pattern as llvm-debuginfod scanning ELF files.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TEMPLATECACHE_DAEMONCACHEBACKEND_H
#define LLVM_CLANG_TEMPLATECACHE_DAEMONCACHEBACKEND_H

#include "clang/Serialization/TemplateInstantiationCache.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include <string>

namespace clang {

class CodeGenOptions;
class ModuleCache;

/// HTTP GET-based cache backend that reads PCM blobs from a remote daemon.
///
/// The daemon URL is set via -ftemplate-instantiation-cache-daemon=<url>.
/// This backend is read-only: store() and storeFunc() are no-ops.
/// The local ASTImporterCacheBackend handles all writes.
class DaemonCacheBackend : public TemplateInstantiationCacheBackend {
public:
  DaemonCacheBackend(std::string DaemonURL, TemplateInstantiationCache &Cache,
                     ModuleCache &ModCache,
                     const PCHContainerReader &PCHReader,
                     const CodeGenOptions &CodeGenOpts);

  ~DaemonCacheBackend() override;

  /// Try to load a class specialization from the daemon via HTTP GET.
  /// Returns true on cache hit (instantiation should be skipped).
  bool tryLoad(Sema &S, ClassTemplateSpecializationDecl *D) override;

  /// No-op: daemon is read-only from the compiler's perspective.
  void store(Sema &S, ClassTemplateSpecializationDecl *D) override {}

  /// Try to load a function specialization from the daemon via HTTP GET.
  bool tryLoadFunc(Sema &S, FunctionDecl *D) override;

  /// No-op: daemon is read-only from the compiler's perspective.
  void storeFunc(Sema &S, FunctionDecl *D) override {}

private:
  /// Fetch a PCM blob from the daemon by hash. Returns empty string on miss.
  std::string fetchBlob(StringRef Hash);

  std::string DaemonURL;
  TemplateInstantiationCache &Cache;
  ModuleCache &ModCache;
  const PCHContainerReader &PCHReader;
  const CodeGenOptions &CodeGenOpts;
};

} // namespace clang

#endif // LLVM_CLANG_TEMPLATECACHE_DAEMONCACHEBACKEND_H
