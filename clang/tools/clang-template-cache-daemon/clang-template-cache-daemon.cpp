//===-- clang-template-cache-daemon.cpp - Template cache HTTP server ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the clang-template-cache-daemon tool, which serves the
/// cross-TU template instantiation cache protocol over HTTP. The tool
/// periodically scans one or more filesystem directories for index.tic files
/// and serves their PCM blobs to compiler processes.
///
/// Design mirrors llvm-debuginfod: TemplateCacheLog (thread-safe message
/// queue), TemplateCacheCollection (directory scanner that populates an
/// in-memory hash→blob map), and TemplateCacheServer (HTTP GET handler).
///
/// Compilers query the daemon via:
///   GET /pcm/<hash>
/// returning the raw PCM blob on hit, 404 on miss.
///
/// Compilers still write new entries to their local index.tic via
/// -ftemplate-instantiation-cache=<dir>. The daemon picks up new entries
/// when it rescans on a periodic or on-demand basis.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/HTTP/HTTPClient.h"
#include "llvm/Support/HTTP/HTTPServer.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/RWMutex.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;

// Tablegen-generated option declarations.
namespace {
enum ID {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

using namespace llvm::opt;
static constexpr opt::OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
};

class TemplateCacheDaemonOptTable : public opt::GenericOptTable {
public:
  TemplateCacheDaemonOptTable()
      : GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {}
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// index.tic binary format constants
// Mirrors TemplateInstantiationCache's on-disk format.
//===----------------------------------------------------------------------===//

static constexpr StringLiteral IndexMagic = "CTIC";
static constexpr uint32_t IndexVersion = 1;
static constexpr size_t HashLen = 64;

/// One entry in the index.tic offset table.
struct IndexEntry {
  char SpecHash[HashLen];   ///< Hex-encoded BLAKE3 hash.
  uint64_t Offset;          ///< Byte offset of blob in data section.
  uint64_t Size;            ///< Byte size of the blob.
  uint8_t Flags;            ///< 0=success, 1=SFINAE failure, 2=delta.
  char BaseHash[HashLen];   ///< Base hash for delta-compressed entries.
};

static_assert(sizeof(IndexEntry) == HashLen + 8 + 8 + 1 + HashLen,
              "IndexEntry size mismatch");

//===----------------------------------------------------------------------===//
// TemplateCacheLog — thread-safe message queue (mirrors DebuginfodLog)
//===----------------------------------------------------------------------===//

struct TemplateCacheLogEntry {
  std::string Message;
  explicit TemplateCacheLogEntry(const Twine &Msg) : Message(Msg.str()) {}
};

class TemplateCacheLog {
public:
  void push(const Twine &Msg) { push(TemplateCacheLogEntry(Msg)); }

  void push(TemplateCacheLogEntry Entry) {
    {
      std::lock_guard<std::mutex> Guard(QueueMutex);
      Queue.push(std::move(Entry));
    }
    QueueCV.notify_one();
  }

  TemplateCacheLogEntry pop() {
    std::unique_lock<std::mutex> Guard(QueueMutex);
    QueueCV.wait(Guard, [this] { return !Queue.empty(); });
    TemplateCacheLogEntry E = std::move(Queue.front());
    Queue.pop();
    return E;
  }

private:
  std::mutex QueueMutex;
  std::condition_variable QueueCV;
  std::queue<TemplateCacheLogEntry> Queue;
};

//===----------------------------------------------------------------------===//
// TemplateCacheCollection — scans index.tic files, serves blobs
// (mirrors DebuginfodCollection)
//===----------------------------------------------------------------------===//

class TemplateCacheCollection {
public:
  TemplateCacheCollection(ArrayRef<StringRef> PathsRef,
                          TemplateCacheLog &Log,
                          ThreadPoolInterface &Pool,
                          double MinInterval)
      : Log(Log), Pool(Pool), MinInterval(MinInterval) {
    for (StringRef P : PathsRef)
      Paths.push_back(P.str());
  }

  /// Rescan all configured paths for new or updated index.tic files.
  Error update();

  /// Rescan if the last scan was more than MinInterval seconds ago.
  Expected<bool> updateIfStale();

  /// Rescan periodically in a loop (runs indefinitely).
  Error updateForever(std::chrono::milliseconds Interval);

  /// Look up a PCM blob by its hash. Returns empty string on miss.
  std::string findBlob(StringRef Hash);

private:
  /// Parse one index.tic file and load its entries into Blobs.
  void loadIndexFile(StringRef FilePath);

  TemplateCacheLog &Log;
  ThreadPoolInterface &Pool;
  double MinInterval;
  std::vector<std::string> Paths;

  /// Guard for UpdateTimer.
  sys::Mutex UpdateMutex;
  Timer UpdateTimer{"update", "Collection update time"};

  /// Guard for Blobs.
  sys::RWMutex BlobsMutex;
  /// In-memory cache: hash (64-char hex string) → PCM blob bytes.
  StringMap<std::string> Blobs;
};

/// Attempt to load every index.tic under the given directory path.
static void scanForIndexFiles(StringRef RootPath,
                              TemplateCacheCollection &Coll,
                              ThreadPoolInterface &Pool,
                              TemplateCacheLog &Log) {
  std::error_code EC;
  sys::fs::recursive_directory_iterator It(Twine(RootPath), EC), End;
  std::mutex ItMutex;
  ThreadPoolTaskGroup TG(Pool);

  for (unsigned W = 0; W < Pool.getMaxConcurrency(); ++W) {
    TG.async([&]() {
      while (true) {
        std::string FilePath;
        {
          std::lock_guard<std::mutex> Guard(ItMutex);
          if (It == End || EC)
            return;
          FilePath = It->path();
          It.increment(EC);
        }
        if (sys::path::filename(FilePath) != "index.tic")
          continue;
        Log.push("Scanning " + FilePath);
        Coll.loadIndexFile(FilePath);
      }
    });
  }
  TG.wait();
}

void TemplateCacheCollection::loadIndexFile(StringRef FilePath) {
  auto BufOrErr =
      MemoryBuffer::getFile(FilePath, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (!BufOrErr)
    return;

  StringRef Data = (*BufOrErr)->getBuffer();
  // Minimum: 4+4+4 = 12 bytes for header.
  if (Data.size() < 12)
    return;

  // Validate magic and version.
  if (Data.substr(0, 4) != IndexMagic)
    return;
  uint32_t Version;
  memcpy(&Version, Data.data() + 4, 4);
  if (Version != IndexVersion)
    return;

  uint32_t NumEntries;
  memcpy(&NumEntries, Data.data() + 8, 4);

  size_t EntriesOffset = 12;
  size_t DataOffset = EntriesOffset + (size_t)NumEntries * sizeof(IndexEntry);
  if (DataOffset > Data.size())
    return;

  std::lock_guard<sys::RWMutex> WriteGuard(BlobsMutex);
  for (uint32_t I = 0; I < NumEntries; ++I) {
    const IndexEntry *E = reinterpret_cast<const IndexEntry *>(
        Data.data() + EntriesOffset + I * sizeof(IndexEntry));

    // Skip SFINAE failure markers (Flags == 1) and delta-compressed entries
    // (Flags == 2, which require a base blob to decompress — skip for now).
    if (E->Flags != 0)
      continue;

    size_t BlobStart = DataOffset + E->Offset;
    size_t BlobEnd = BlobStart + E->Size;
    if (BlobEnd > Data.size())
      continue;

    std::string Hash(E->SpecHash, HashLen);
    // Only insert if not already present (first writer wins).
    Blobs.try_emplace(Hash, Data.substr(BlobStart, E->Size).str());
  }
}

std::string TemplateCacheCollection::findBlob(StringRef Hash) {
  std::shared_lock<sys::RWMutex> ReadGuard(BlobsMutex);
  auto It = Blobs.find(Hash);
  if (It != Blobs.end())
    return It->second;
  return {};
}

Error TemplateCacheCollection::update() {
  std::lock_guard<sys::Mutex> Guard(UpdateMutex);
  if (UpdateTimer.isRunning())
    UpdateTimer.stopTimer();
  UpdateTimer.startTimer();

  for (const std::string &Path : Paths)
    scanForIndexFiles(Path, *this, Pool, Log);

  UpdateTimer.stopTimer();
  Log.push("Collection updated: " + Twine(Blobs.size()) + " entries.");
  return Error::success();
}

Expected<bool> TemplateCacheCollection::updateIfStale() {
  if (!UpdateTimer.isRunning())
    return false;
  UpdateTimer.stopTimer();
  double Elapsed = UpdateTimer.getTotalTime().getWallTime();
  UpdateTimer.startTimer();
  if (Elapsed < MinInterval)
    return false;
  if (Error E = update())
    return std::move(E);
  return true;
}

Error TemplateCacheCollection::updateForever(
    std::chrono::milliseconds Interval) {
  while (true) {
    if (Error E = update())
      return E;
    std::this_thread::sleep_for(Interval);
  }
}

//===----------------------------------------------------------------------===//
// TemplateCacheServer — registers HTTP GET /pcm/<hash>
// (mirrors DebuginfodServer)
//===----------------------------------------------------------------------===//

struct TemplateCacheServer {
  HTTPServer Server;
  TemplateCacheLog &Log;
  TemplateCacheCollection &Collection;

  TemplateCacheServer(TemplateCacheLog &Log,
                      TemplateCacheCollection &Collection)
      : Log(Log), Collection(Collection) {
    cantFail(Server.get(R"(/pcm/(.+))", [&](HTTPServerRequest Request) {
      StringRef Hash = Request.UrlPathMatches[0];
      Log.push("GET /pcm/" + Hash.str());

      // On-demand rescan if the entry is not found and data might be stale.
      std::string Blob = Collection.findBlob(Hash);
      if (Blob.empty()) {
        Expected<bool> Updated = Collection.updateIfStale();
        if (Updated && *Updated)
          Blob = Collection.findBlob(Hash);
        else if (!Updated)
          consumeError(Updated.takeError());
      }

      if (Blob.empty()) {
        Request.setResponse({404, "text/plain", "Hash not found\n"});
        return;
      }

      // Serve the PCM blob directly from memory.
      // Use a streaming response so we don't copy the bytes again.
      std::string *BlobPtr = new std::string(std::move(Blob));
      Request.setResponse(
          {200, "application/octet-stream", BlobPtr->size(),
           [BlobPtr](size_t Off, size_t Len) -> StringRef {
             return StringRef(*BlobPtr).substr(Off, Len);
           },
           [BlobPtr](bool) { delete BlobPtr; }});
    }));
  }
};

//===----------------------------------------------------------------------===//
// Global options and main
//===----------------------------------------------------------------------===//

static unsigned Port;
static std::string HostInterface;
static int ScanInterval;
static double MinInterval;
static size_t MaxConcurrency;
static bool VerboseLogging;
static std::vector<std::string> ScanPaths;

ExitOnError ExitOnErr;

template <typename T>
static void parseIntArg(const opt::InputArgList &Args, int ID, T &Value,
                        T Default) {
  if (const opt::Arg *A = Args.getLastArg(ID)) {
    StringRef V(A->getValue());
    if (!llvm::to_integer(V, Value, 0)) {
      errs() << A->getSpelling() + ": expected an integer, but got '" + V +
                    "'";
      exit(1);
    }
  } else {
    Value = Default;
  }
}

static void parseArgs(int Argc, char **Argv) {
  TemplateCacheDaemonOptTable Tbl;
  llvm::StringRef ToolName = Argv[0];
  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver{A};
  opt::InputArgList Args =
      Tbl.parseArgs(Argc, Argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        llvm::errs() << Msg << '\n';
        std::exit(1);
      });

  if (Args.hasArg(OPT_help)) {
    Tbl.printHelp(llvm::outs(),
                  "clang-template-cache-daemon [options] <cache directories>",
                  ToolName.str().c_str());
    std::exit(0);
  }

  VerboseLogging = Args.hasArg(OPT_verbose_logging);
  ScanPaths = Args.getAllArgValues(OPT_INPUT);

  parseIntArg(Args, OPT_port, Port, 0u);
  parseIntArg(Args, OPT_scan_interval, ScanInterval, 300);
  parseIntArg(Args, OPT_max_concurrency, MaxConcurrency, size_t(0));

  if (const opt::Arg *A = Args.getLastArg(OPT_min_interval)) {
    StringRef V(A->getValue());
    if (!llvm::to_float(V, MinInterval)) {
      errs() << A->getSpelling() + ": expected a number, but got '" + V + "'";
      exit(1);
    }
  } else {
    MinInterval = 10.0;
  }

  HostInterface = Args.getLastArgValue(OPT_host_interface, "0.0.0.0");
}

int clang_template_cache_daemon_main(int Argc, char **Argv,
                                     const llvm::ToolContext &) {
  parseArgs(Argc, Argv);

  if (!HTTPServer::isAvailable()) {
    errs() << "clang-template-cache-daemon: LLVM was not built with HTTP "
              "server support (requires LLVM_ENABLE_HTTPLIB=ON)\n";
    return 1;
  }

  SmallVector<StringRef, 4> Paths;
  for (const std::string &P : ScanPaths)
    Paths.push_back(P);

  DefaultThreadPool Pool(hardware_concurrency(MaxConcurrency));
  TemplateCacheLog Log;
  TemplateCacheCollection Collection(Paths, Log, Pool, MinInterval);
  TemplateCacheServer Server(Log, Collection);

  if (!Port)
    Port = ExitOnErr(Server.Server.bind(HostInterface.c_str()));
  else
    ExitOnErr(Server.Server.bind(Port, HostInterface.c_str()));

  Log.push("Listening on port " + Twine(Port).str());

  // Background thread: print log messages.
  Pool.async([&]() {
    while (true) {
      TemplateCacheLogEntry Entry = Log.pop();
      if (VerboseLogging) {
        outs() << Entry.Message << "\n";
        outs().flush();
      }
    }
  });

  // Background thread: periodic rescan of cache directories.
  if (Paths.size() && ScanInterval > 0) {
    Pool.async([&]() {
      ExitOnErr(Collection.updateForever(
          std::chrono::seconds(ScanInterval)));
    });
  } else if (Paths.size()) {
    // Initial load only (no periodic rescan).
    ExitOnErr(Collection.update());
  }

  // Serve HTTP requests (blocks until stop() is called).
  ExitOnErr(Server.Server.listen());
  Pool.wait();
  llvm_unreachable("The server should never stop listening.");
}
