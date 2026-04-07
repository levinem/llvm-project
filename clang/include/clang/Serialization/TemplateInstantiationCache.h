//===- TemplateInstantiationCache.h - Cross-TU template cache ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the TemplateInstantiationCache class, which provides
// cross-translation-unit caching of template specialization instantiations.
//
// When enabled via -ftemplate-instantiation-cache=<path>, the first TU to
// instantiate a template specialization writes the result to an on-disk cache.
// Subsequent TUs can read from the cache instead of re-instantiating,
// significantly reducing compile time for template-heavy C++ code.
//
// The cache uses a single index file per context-hash directory:
//
//   <base-path>/<context-hash>/index.tic
//
// The index file contains an offset table mapping specialization hashes to
// embedded PCM blobs, accessed via mmap for I/O efficiency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SERIALIZATION_TEMPLATEINSTANTIATIONCACHE_H
#define LLVM_CLANG_SERIALIZATION_TEMPLATEINSTANTIATIONCACHE_H

#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clang {

class ASTContext;
class ASTUnit;
class FileManager;
class Sema;

/// Cross-TU template instantiation cache.
///
/// Uses a single index file with mmap for fast lookups. Each entry contains
/// a serialized PCM blob produced by ASTWriter via a temporary ASTContext.
/// On cache hit, ASTReader loads the blob and ASTImporter merges the
/// specialization into the consuming TU.
class TemplateInstantiationCache {
public:
  TemplateInstantiationCache(StringRef CachePath, StringRef ContextHash,
                             FileManager &FM);
  ~TemplateInstantiationCache();

  /// Try to load a class template specialization from the cache.
  bool tryLoadClassSpecialization(Sema &S,
                                 ClassTemplateSpecializationDecl *D);

  /// Try to load a function template specialization from the cache.
  bool tryLoadFunctionSpecialization(Sema &S, FunctionDecl *D);

  /// Store a successfully instantiated class specialization in the cache.
  void storeClassSpecialization(Sema &S,
                                ClassTemplateSpecializationDecl *D);

  /// Store a successfully instantiated function specialization in the cache.
  void storeFunctionSpecialization(Sema &S, FunctionDecl *D);

  // --- Step 9: Negative caching (SFINAE failures) ---

  /// Check if a SFINAE failure is cached for the given hash.
  /// Returns true if a failure is cached (caller should skip substitution).
  bool isSFINAEFailureCached(StringRef Hash);

  /// Record a SFINAE failure in the cache.
  void storeSFINAEFailure(StringRef Hash);

  // --- Step 10: Hierarchical dependency caching ---

  /// Record that specialization DepHash is a dependency of SpecHash.
  /// Used to import dependencies before the main specialization.
  void recordDependency(StringRef SpecHash, StringRef DepHash);

  /// Get the list of dependency hashes for a specialization.
  /// Returns empty if no dependencies recorded.
  SmallVector<std::string, 4> getDependencies(StringRef SpecHash);

  // --- Step 11: Pre-populated STL cache ---

  /// Try to load from a pre-populated STL cache shipped with the compiler.
  /// Falls back to this when user cache misses.
  bool tryLoadFromSTLCache(Sema &S, StringRef SpecHash, bool IsClass);

  /// Set the path to the pre-populated STL cache directory.
  void setSTLCachePath(StringRef Path) { STLCachePath = std::string(Path); }

  /// Flush any pending writes to the index file.
  void flushPendingWrites();

  /// Print cache statistics to the given stream.
  void printStats(raw_ostream &OS) const;

  bool isEnabled() const { return Enabled; }

private:
  /// Flags for index entries.
  enum EntryFlags : uint8_t {
    EF_Success = 0,          ///< Successful instantiation (PCM blob present).
    EF_SFINAEFailure = 1,    ///< SFINAE failure (no PCM blob, just marker).
    EF_DeltaCompressed = 2,  ///< PCM blob is delta-compressed against base.
  };

  /// An entry in the index file's offset table.
  struct IndexEntry {
    char SpecHash[64];   ///< BLAKE3 hex hash of the specialization.
    uint64_t Offset;     ///< Byte offset into the data section.
    uint64_t Size;       ///< Size of the PCM blob in bytes (0 for failures).
    uint8_t Flags;       ///< EntryFlags.
    char BaseHash[64];   ///< For delta: hash of the base specialization.
  };

  /// A pending write: PCM blob to be appended to the index on flush.
  struct PendingEntry {
    std::string SpecHash;
    SmallVector<char, 0> PCMData;
    uint8_t Flags = EF_Success;
    std::string BaseHash;  ///< For delta compression.
  };

  /// Dependency relationship: SpecHash depends on DepHashes.
  struct DependencyRecord {
    std::string SpecHash;
    SmallVector<std::string, 4> DepHashes;
  };

  /// Compute a stable hash for a class template specialization.
  std::string computeClassSpecHash(ClassTemplateSpecializationDecl *D,
                                   ASTContext &Ctx);

  /// Compute a stable hash for a function template specialization.
  std::string computeFuncSpecHash(FunctionDecl *D, ASTContext &Ctx);

  /// Serialize a specialization to a PCM blob via ASTImporter + temp context.
  bool serializeToPCM(Sema &S, Decl *D, SmallVectorImpl<char> &Out);

  /// Look up a specialization hash in the index. Returns the PCM blob
  /// as a MemoryBuffer, or nullptr on miss.
  std::unique_ptr<llvm::MemoryBuffer> lookupInIndex(StringRef SpecHash);

  /// Load the cached PCM and import the specialization into the consuming TU.
  bool importFromPCM(Sema &S, llvm::MemoryBufferRef PCMData, bool IsClass);

  /// Load the index file into memory (mmap).
  bool loadIndex();

  /// Write the full index file (header + offset table + data).
  bool writeIndex();

  /// Build the path to the index file.
  std::string getIndexPath() const;

  /// Atomically write a buffer to a file path.
  std::error_code atomicWrite(StringRef Path, StringRef Contents);

  /// Load the pre-populated STL cache index.
  bool loadSTLIndex();

  /// Look up a hash in the STL cache index.
  std::unique_ptr<llvm::MemoryBuffer> lookupInSTLIndex(StringRef SpecHash);

  /// Delta-compress a PCM blob against a base blob.
  /// Returns the compressed data, or empty if compression isn't profitable.
  SmallVector<char, 0> deltaCompress(ArrayRef<char> Data,
                                      ArrayRef<char> BaseData);

  /// Delta-decompress a blob using the base blob.
  SmallVector<char, 0> deltaDecompress(ArrayRef<char> DeltaData,
                                        ArrayRef<char> BaseData);

  /// Get the template identity string for grouping delta compression.
  std::string getTemplateIdentity(Decl *D, ASTContext &Ctx);

  /// The fully qualified cache directory.
  std::string CacheDir;

  /// FileManager for filesystem operations.
  FileManager &FileMgr;

  /// Whether the cache is operational.
  bool Enabled = false;

  /// Whether the index has been loaded.
  bool IndexLoaded = false;

  /// The mmap'd index file (kept alive for the duration of the TU).
  std::unique_ptr<llvm::MemoryBuffer> IndexBuffer;

  /// Parsed index entries (from loaded index file).
  std::vector<IndexEntry> IndexEntries;

  /// Fast lookup: hash string → index into IndexEntries.
  llvm::DenseMap<StringRef, unsigned> IndexMap;

  /// Entries to write at end of TU.
  std::vector<PendingEntry> PendingWrites;

  /// Set of hashes we've already seen (avoid redundant lookups).
  llvm::DenseSet<std::string> KnownHashes;

  /// Dependency graph: spec hash → list of dependency hashes.
  llvm::DenseMap<std::string, SmallVector<std::string, 4>> Dependencies;

  /// Path to pre-populated STL cache (shipped with compiler).
  std::string STLCachePath;

  /// Loaded STL cache index (lazily initialized).
  bool STLIndexLoaded = false;
  std::unique_ptr<llvm::MemoryBuffer> STLIndexBuffer;
  std::vector<IndexEntry> STLIndexEntries;
  llvm::DenseMap<StringRef, unsigned> STLIndexMap;

  /// Template-identity groups for delta compression.
  /// Maps template qualified name → hash of first (base) specialization.
  llvm::DenseMap<std::string, std::string> DeltaBaseMap;

  // Statistics
  unsigned CacheHits = 0;
  unsigned CacheMisses = 0;
  unsigned CacheWrites = 0;
  unsigned CacheErrors = 0;
  unsigned SFINAECacheHits = 0;
  unsigned STLCacheHits = 0;
  unsigned DeltaCompressions = 0;
};

} // namespace clang

#endif // LLVM_CLANG_SERIALIZATION_TEMPLATEINSTANTIATIONCACHE_H
