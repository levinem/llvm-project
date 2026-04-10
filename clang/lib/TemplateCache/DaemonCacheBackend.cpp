//===- DaemonCacheBackend.cpp - HTTP daemon cache backend -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements DaemonCacheBackend: HTTP GET of PCM blobs from a remote
// clang-template-cache-daemon. Uses LLVM's HTTPClient (GET-only) since the
// daemon is read-only from the compiler's perspective.
//
// See DaemonCacheBackend.h for architecture overview.
//
//===----------------------------------------------------------------------===//

#include "clang/TemplateCache/DaemonCacheBackend.h"
#include "clang/TemplateCache/ASTImporterCacheBackend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/TemplateInstantiationCache.h"
#include "llvm/Support/Error.h"

#ifdef LLVM_ENABLE_CURL
#include "llvm/Support/HTTP/HTTPClient.h"
#endif

using namespace clang;

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

DaemonCacheBackend::DaemonCacheBackend(
    std::string DaemonURL, TemplateInstantiationCache &Cache,
    ModuleCache &ModCache, const PCHContainerReader &PCHReader,
    const CodeGenOptions &CodeGenOpts)
    : DaemonURL(std::move(DaemonURL)), Cache(Cache), ModCache(ModCache),
      PCHReader(PCHReader), CodeGenOpts(CodeGenOpts) {}

DaemonCacheBackend::~DaemonCacheBackend() = default;

//===----------------------------------------------------------------------===//
// HTTP GET helper
//===----------------------------------------------------------------------===//

std::string DaemonCacheBackend::fetchBlob(StringRef Hash) {
#ifdef LLVM_ENABLE_CURL
  if (!llvm::HTTPClient::isAvailable())
    return {};

  std::string URL = DaemonURL + "/pcm/" + Hash.str();

  // Accumulate response body chunks.
  std::string Body;
  class BlobHandler : public llvm::HTTPResponseHandler {
  public:
    std::string &Dest;
    explicit BlobHandler(std::string &D) : Dest(D) {}
    llvm::Error handleBodyChunk(llvm::StringRef Chunk) override {
      Dest.append(Chunk.begin(), Chunk.end());
      return llvm::Error::success();
    }
  };
  BlobHandler Handler(Body);

  llvm::HTTPClient Client;
  Client.setTimeout(std::chrono::milliseconds(500));
  llvm::HTTPRequest Request(URL);

  if (llvm::Error E = Client.perform(Request, Handler)) {
    llvm::consumeError(std::move(E));
    return {};
  }

  if (Client.responseCode() != 200)
    return {};

  return Body;
#else
  (void)Hash;
  return {};
#endif
}

//===----------------------------------------------------------------------===//
// tryLoad / tryLoadFunc
//===----------------------------------------------------------------------===//

bool DaemonCacheBackend::tryLoad(Sema &S,
                                  ClassTemplateSpecializationDecl *D) {
  std::string Hash =
      TemplateInstantiationCache::computeClassSpecHash(D, S.getASTContext());

  std::string Blob = fetchBlob(Hash);
  if (Blob.empty()) {
    Cache.recordMiss();
    return false;
  }

  bool Ok = deserializeCacheBlob(S, Blob, /*IsClass=*/true, D,
                                  ModCache, PCHReader, CodeGenOpts);
  if (Ok) {
    Cache.recordDaemonHit();
    return true;
  }

  Cache.recordError();
  return false;
}

bool DaemonCacheBackend::tryLoadFunc(Sema &S, FunctionDecl *D) {
  std::string Hash =
      TemplateInstantiationCache::computeFuncSpecHash(D, S.getASTContext());

  std::string Blob = fetchBlob(Hash);
  if (Blob.empty()) {
    Cache.recordMiss();
    return false;
  }

  bool Ok = deserializeCacheBlob(S, Blob, /*IsClass=*/false, D,
                                  ModCache, PCHReader, CodeGenOpts);
  if (Ok) {
    Cache.recordDaemonHit();
    return true;
  }

  Cache.recordError();
  return false;
}
