//===- TemplateInstantiationCache.h - Cross-TU template cache ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the TemplateInstantiationCache class, which provides
// the storage and index layer for cross-TU template specialization caching.
//
// The cache stores PCM blobs in a single mmap'd index file per context-hash
// directory. The Sema layer is responsible for producing and consuming the
// PCM blobs via ASTImporter; this class only handles storage, lookup,
// hashing, and compression.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SERIALIZATION_TEMPLATEINSTANTIATIONCACHE_H
#define LLVM_CLANG_SERIALIZATION_TEMPLATEINSTANTIATIONCACHE_H

#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>
#include <string>
#include <vector>

namespace clang {

class ASTContext;
class FileManager;

/// Cross-TU template instantiation cache — storage and index layer.
///
/// Manages the on-disk index file that maps specialization hashes to
/// serialized PCM blobs. The Sema layer handles ASTImporter-based
/// serialization/deserialization and calls into this class for storage.

/// Abstract backend interface for serializing and deserializing template
/// specializations. The concrete implementation (ASTImporterCacheBackend)
/// lives in clangTemplateCache, which can freely link clangFrontend.
/// Registered via setBackend() by ExecuteCompilerInvocation.
class TemplateInstantiationCacheBackend {
public:
  virtual ~TemplateInstantiationCacheBackend() = default;

  /// Try to load a class specialization from the cache into the consuming TU.
  /// Returns true on cache hit (instantiation should be skipped).
  virtual bool tryLoad(Sema &S, ClassTemplateSpecializationDecl *D) = 0;

  /// Store a successfully instantiated class specialization into the cache.
  virtual void store(Sema &S, ClassTemplateSpecializationDecl *D) = 0;

  /// Try to load a function specialization from the cache.
  /// Returns true on cache hit (instantiation should be skipped).
  virtual bool tryLoadFunc(Sema &S, FunctionDecl *D) = 0;

  /// Store a successfully instantiated function specialization into the cache.
  virtual void storeFunc(Sema &S, FunctionDecl *D) = 0;
};

class TemplateInstantiationCache {
public:
  TemplateInstantiationCache(StringRef CachePath, StringRef ContextHash,
                             FileManager &FM);
  ~TemplateInstantiationCache();

  // --- Backend (ASTImporter-based serialization) ---

  /// Set the concrete backend that handles PCM serialization/deserialization.
  /// Must be called before the first cache lookup/store.
  void setBackend(std::unique_ptr<TemplateInstantiationCacheBackend> B) {
    Backend = std::move(B);
  }

  /// Get the backend, or nullptr if not yet set.
  TemplateInstantiationCacheBackend *getBackend() { return Backend.get(); }

  // --- Lookup ---

  /// Look up a PCM blob by specialization hash.
  /// Returns the blob as a MemoryBuffer, or nullptr on cache miss.
  std::unique_ptr<llvm::MemoryBuffer> lookup(StringRef SpecHash);

  /// Check if a SFINAE failure is cached for the given hash.
  bool isSFINAEFailureCached(StringRef SpecHash);

  // --- Store ---

  /// Queue a PCM blob for storage. Written to disk on flushPendingWrites().
  void store(StringRef SpecHash, ArrayRef<char> PCMData);

  /// Record a SFINAE failure (zero-size marker entry).
  void storeSFINAEFailure(StringRef SpecHash);

  /// Flush all pending writes to the index file.
  void flushPendingWrites();

  // --- Hash computation ---

  /// Compute a stable hash for a class template specialization.
  static std::string computeClassSpecHash(ClassTemplateSpecializationDecl *D,
                                          ASTContext &Ctx);

  /// Compute a stable hash for a function template specialization.
  static std::string computeFuncSpecHash(FunctionDecl *D, ASTContext &Ctx);

  // --- Dependency tracking ---

  /// Record that SpecHash depends on DepHash.
  void recordDependency(StringRef SpecHash, StringRef DepHash);

  /// Get dependencies for a specialization.
  SmallVector<std::string, 4> getDependencies(StringRef SpecHash);

  // --- STL cache ---

  /// Set path to pre-populated STL cache.
  void setSTLCachePath(StringRef Path) { STLCachePath = std::string(Path); }

  /// Look up in STL cache as fallback.
  std::unique_ptr<llvm::MemoryBuffer> lookupSTL(StringRef SpecHash);

  // --- Statistics ---

  void recordHit() { ++CacheHits; }
  void recordMiss() { ++CacheMisses; }
  void recordError() { ++CacheErrors; }
  void recordSFINAEHit() { ++SFINAECacheHits; }
  void recordSTLHit() { ++STLCacheHits; }
  /// Record a cache hit served by the remote daemon (subset of CacheHits).
  void recordDaemonHit() { ++CacheHits; ++DaemonHits; }

  void printStats(raw_ostream &OS) const;

  bool isEnabled() const { return Enabled; }

private:
  /// Entry flags in the index.
  enum EntryFlags : uint8_t {
    EF_Success = 0,
    EF_SFINAEFailure = 1,
    EF_DeltaCompressed = 2,
  };

  /// Index entry.
  struct IndexEntry {
    char SpecHash[64];
    uint64_t Offset;
    uint64_t Size;
    uint8_t Flags;
    char BaseHash[64];
  };

  /// Pending write entry.
  struct PendingEntry {
    std::string SpecHash;
    SmallVector<char, 0> PCMData;
    uint8_t Flags = EF_Success;
    std::string BaseHash;
  };

  bool loadIndex();
  bool writeIndex();
  bool loadSTLIndex();
  std::string getIndexPath() const;
  std::error_code atomicWrite(StringRef Path, StringRef Contents);

  // Delta compression helpers.
  SmallVector<char, 0> deltaCompress(ArrayRef<char> Data,
                                      ArrayRef<char> BaseData);
  SmallVector<char, 0> deltaDecompress(ArrayRef<char> DeltaData,
                                        ArrayRef<char> BaseData);

  std::string CacheDir;
  FileManager &FileMgr;
  bool Enabled = false;

  /// The concrete backend for PCM serialization/deserialization.
  std::unique_ptr<TemplateInstantiationCacheBackend> Backend;
  bool IndexLoaded = false;

  std::unique_ptr<llvm::MemoryBuffer> IndexBuffer;
  std::vector<IndexEntry> IndexEntries;
  llvm::DenseMap<StringRef, unsigned> IndexMap;

  std::vector<PendingEntry> PendingWrites;
  llvm::DenseSet<std::string> KnownHashes;

  // Dependencies.
  llvm::DenseMap<std::string, SmallVector<std::string, 4>> Dependencies;

  // STL cache.
  std::string STLCachePath;
  bool STLIndexLoaded = false;
  std::unique_ptr<llvm::MemoryBuffer> STLIndexBuffer;
  std::vector<IndexEntry> STLIndexEntries;
  llvm::DenseMap<StringRef, unsigned> STLIndexMap;

  // Delta compression base map.
  llvm::DenseMap<std::string, std::string> DeltaBaseMap;

  // Statistics.
  unsigned CacheHits = 0;
  unsigned CacheMisses = 0;
  unsigned CacheWrites = 0;
  unsigned CacheErrors = 0;
  unsigned SFINAECacheHits = 0;
  unsigned STLCacheHits = 0;
  unsigned DaemonHits = 0;   ///< Subset of CacheHits served by daemon.
  unsigned DeltaCompressions = 0;
};

} // namespace clang

#endif // LLVM_CLANG_SERIALIZATION_TEMPLATEINSTANTIATIONCACHE_H
