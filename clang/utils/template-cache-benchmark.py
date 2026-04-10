#!/usr/bin/env python3
"""
template-cache-benchmark.py - Benchmark the template instantiation cache.

Compiles a template-heavy source file multiple times using -ftime-trace to
measure how much time is spent on template instantiation with and without
the cache. Reports speedup metrics.

Usage:
  python3 template-cache-benchmark.py --clang /path/to/clang [options]

Example:
  python3 template-cache-benchmark.py \\
      --clang build/bin/clang \\
      --cache-dir /tmp/tic-bench \\
      --runs 5 \\
      --std c++17

The script:
  1. Compiles a representative template-heavy file once (cold cache, writes entries)
  2. Compiles it N more times (warm cache, reads entries)
  3. Parses -ftime-trace JSON to extract InstantiateClass/InstantiateFunction times
  4. Reports: cold time, warm avg time, speedup ratio, hit rate
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from statistics import mean, median, stdev


# Representative template-heavy source that exercises common patterns.
BENCHMARK_SOURCE = r"""
// Template instantiation cache benchmark source.
// Uses common STL-like patterns to exercise the cache.

#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <tuple>

// Custom templates that will be cached independently of STL.
template <typename T>
struct Ring {
    T buf[64];
    unsigned head = 0, tail = 0;
    void push(T v) { buf[tail++ % 64] = v; }
    T pop() { return buf[head++ % 64]; }
    bool empty() const { return head == tail; }
};

template <typename K, typename V>
struct LRUCache {
    std::map<K, V> data;
    std::vector<K> order;
    unsigned cap;
    explicit LRUCache(unsigned c) : cap(c) {}
    void put(K k, V v) {
        data[k] = v;
        order.push_back(k);
        if (order.size() > cap) {
            data.erase(order.front());
            order.erase(order.begin());
        }
    }
    std::optional<V> get(K k) const {
        auto it = data.find(k);
        if (it == data.end()) return std::nullopt;
        return it->second;
    }
};

template <typename T, typename F>
std::vector<T> filter(const std::vector<T>& v, F pred) {
    std::vector<T> out;
    std::copy_if(v.begin(), v.end(), std::back_inserter(out), pred);
    return out;
}

template <typename T, typename F, typename R>
R fold(const std::vector<T>& v, R init, F fn) {
    R acc = init;
    for (const auto& x : v) acc = fn(acc, x);
    return acc;
}

void benchmark_usage() {
    // STL instantiations (common types, should be cached across TUs).
    std::vector<int> vi = {1, 2, 3, 4, 5};
    std::vector<double> vd = {1.0, 2.0, 3.0};
    std::vector<std::string> vs = {"a", "b", "c"};

    std::map<int, std::string> mis;
    mis[1] = "one";
    std::map<std::string, int> msi;
    msi["one"] = 1;

    std::unordered_map<int, double> umid;
    umid[1] = 1.0;
    std::unordered_map<std::string, int> umsi;
    umsi["a"] = 1;

    auto sp = std::make_shared<std::vector<int>>(vi);
    auto up = std::make_unique<std::map<int, std::string>>(mis);

    std::optional<int> oi = 42;
    std::optional<std::string> os = "hello";

    std::variant<int, double, std::string> var = 42;

    std::tuple<int, double, std::string> t(1, 2.0, "three");

    // Custom template instantiations.
    Ring<int> ri;
    ri.push(1); (void)ri.pop();

    Ring<std::string> rs;
    rs.push("x"); (void)rs.pop();

    LRUCache<int, std::string> lru(10);
    lru.put(1, "one");
    (void)lru.get(1);

    LRUCache<std::string, int> lru2(10);
    lru2.put("a", 1);

    auto even = filter(vi, [](int x){ return x % 2 == 0; });
    auto sum = fold(vi, 0, [](int a, int b){ return a + b; });

    std::sort(vi.begin(), vi.end());
    std::sort(vd.begin(), vd.end(), std::greater<double>());
    std::sort(vs.begin(), vs.end());

    (void)even; (void)sum; (void)sp; (void)up;
    (void)oi; (void)os; (void)var; (void)t;
    (void)lru2;
}
"""


def parse_ftime_trace(trace_path: Path) -> dict:
    """Parse a -ftime-trace JSON file and extract template instantiation data."""
    with open(trace_path) as f:
        data = json.load(f)

    total_instantiate_us = 0
    instantiate_class_us = 0
    instantiate_func_us = 0
    cache_hits = 0
    events = data.get("traceEvents", [])

    for event in events:
        name = event.get("name", "")
        dur = event.get("dur", 0)
        ph = event.get("ph", "")

        if ph == "X":  # Complete events
            if name == "InstantiateClass":
                instantiate_class_us += dur
                total_instantiate_us += dur
            elif name == "InstantiateFunction":
                instantiate_func_us += dur
                total_instantiate_us += dur
        elif ph == "i":  # Instant events
            if name == "TemplateInstantiationCacheHit":
                cache_hits += 1

    return {
        "total_instantiate_ms": total_instantiate_us / 1000.0,
        "instantiate_class_ms": instantiate_class_us / 1000.0,
        "instantiate_func_ms": instantiate_func_us / 1000.0,
        "cache_hits": cache_hits,
    }


def compile_with_trace(clang: str, source: Path, cache_dir: str,
                       trace_path: Path, std: str,
                       extra_args: list = None,
                       daemon_url: str = None) -> dict:
    """Compile source with -ftime-trace and return timing data."""
    cmd = [
        clang,
        f"-std={std}",
        "-fsyntax-only",
        f"-ftemplate-instantiation-cache={cache_dir}",
        "-ftemplate-instantiation-cache-stats",
        f"-ftime-trace={trace_path}",
        "-ftime-trace-granularity=0",  # Capture all events
        str(source),
    ]
    if daemon_url:
        cmd.append(f"-ftemplate-instantiation-cache-daemon={daemon_url}")
    if extra_args:
        cmd.extend(extra_args)

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR: Compilation failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)

    # Extract cache stats from stderr.
    stats = {"cache_hits": 0, "cache_misses": 0, "cache_writes": 0,
             "hit_rate": 0.0, "daemon_hits": 0, "daemon_share": 0.0,
             "est_saved_ms": 0.0}
    for line in result.stderr.splitlines():
        if "Total hits:" in line:
            stats["cache_hits"] = int(line.split(":")[-1].strip())
        elif "Cache misses:" in line:
            stats["cache_misses"] = int(line.split(":")[-1].strip())
        elif "Cache writes:" in line:
            stats["cache_writes"] = int(line.split(":")[-1].strip())
        elif "Hit rate:" in line:
            try:
                stats["hit_rate"] = float(
                    line.split(":")[-1].strip().rstrip("%"))
            except ValueError:
                pass
        elif "Daemon hits:" in line:
            try:
                stats["daemon_hits"] = int(line.split(":")[-1].strip())
            except ValueError:
                pass
        elif "Daemon share:" in line:
            try:
                stats["daemon_share"] = float(
                    line.split(":")[-1].strip().rstrip("%"))
            except ValueError:
                pass
        elif "Est. time saved:" in line:
            try:
                val = line.split(":")[-1].strip().split("ms")[0]
                stats["est_saved_ms"] = float(val)
            except (ValueError, IndexError):
                pass

    # Parse trace file.
    timing = parse_ftime_trace(trace_path)
    stats.update(timing)
    return stats


def format_ms(ms: float) -> str:
    if ms < 1.0:
        return f"{ms*1000:.0f}µs"
    elif ms < 1000.0:
        return f"{ms:.1f}ms"
    else:
        return f"{ms/1000:.2f}s"


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark the Clang template instantiation cache.")
    parser.add_argument("--clang", required=True,
                        help="Path to the clang binary")
    parser.add_argument("--cache-dir", default=None,
                        help="Cache directory (default: temp dir, cleaned up)")
    parser.add_argument("--runs", type=int, default=5,
                        help="Number of warm-cache runs (default: 5)")
    parser.add_argument("--std", default="c++17",
                        help="C++ standard (default: c++17)")
    parser.add_argument("--keep-cache", action="store_true",
                        help="Don't clean up the cache directory after running")
    parser.add_argument("--source", default=None,
                        help="Custom source file to benchmark (default: built-in)")
    parser.add_argument("--daemon-url", default=None,
                        help="URL of a running clang-template-cache-daemon "
                             "(e.g., http://localhost:8123). When set, adds a "
                             "third 'daemon warm' row to the results table.")
    args = parser.parse_args()

    # Validate clang.
    if not os.path.isfile(args.clang):
        print(f"ERROR: clang not found at: {args.clang}", file=sys.stderr)
        sys.exit(1)

    # Create working directory.
    workdir = tempfile.mkdtemp(prefix="tic-bench-")
    cleanup_workdir = True

    cache_dir = args.cache_dir
    cleanup_cache = False
    if cache_dir is None:
        cache_dir = os.path.join(workdir, "cache")
        cleanup_cache = not args.keep_cache
    os.makedirs(cache_dir, exist_ok=True)

    try:
        # Write benchmark source.
        if args.source:
            source_path = Path(args.source)
        else:
            source_path = Path(workdir) / "bench.cpp"
            source_path.write_text(BENCHMARK_SOURCE)

        print(f"Template Instantiation Cache Benchmark")
        print(f"=" * 50)
        print(f"Clang:      {args.clang}")
        print(f"Standard:   -std={args.std}")
        print(f"Runs:       1 cold + {args.runs} warm"
              + (f" + {args.runs} daemon warm" if args.daemon_url else ""))
        print(f"Cache dir:  {cache_dir}")
        print(f"Source:     {source_path}")
        if args.daemon_url:
            print(f"Daemon URL: {args.daemon_url}")
        print()

        # Cold run (populates cache).
        print("Running cold compilation (populates cache)...")
        cold_trace = Path(workdir) / "cold.json"
        cold_stats = compile_with_trace(
            args.clang, source_path, cache_dir, cold_trace, args.std)

        print(f"  Template instantiation time: "
              f"{format_ms(cold_stats['total_instantiate_ms'])}")
        print(f"    InstantiateClass:    "
              f"{format_ms(cold_stats['instantiate_class_ms'])}")
        print(f"    InstantiateFunction: "
              f"{format_ms(cold_stats['instantiate_func_ms'])}")
        print(f"  Cache writes: {cold_stats['cache_writes']}")
        print(f"  Cache hits:   {cold_stats['cache_hits']} (expected: 0)")
        print()

        # Warm runs (local disk only).
        print(f"Running {args.runs} warm compilations (local disk cache)...")
        warm_times = []
        warm_hit_rates = []
        warm_hits = []

        for i in range(args.runs):
            trace = Path(workdir) / f"warm_{i}.json"
            stats = compile_with_trace(
                args.clang, source_path, cache_dir, trace, args.std)
            warm_times.append(stats["total_instantiate_ms"])
            warm_hit_rates.append(stats["hit_rate"])
            warm_hits.append(stats["cache_hits"])
            print(f"  Run {i+1}/{args.runs}: "
                  f"{format_ms(stats['total_instantiate_ms'])} "
                  f"(hits: {stats['cache_hits']}, "
                  f"rate: {stats['hit_rate']:.1f}%)")

        # Daemon warm runs (if daemon URL provided).
        daemon_times = []
        daemon_hit_rates = []
        daemon_hits_per_run = []
        daemon_shares = []

        if args.daemon_url:
            print(f"\nRunning {args.runs} daemon warm compilations "
                  f"(daemon: {args.daemon_url})...")
            # Use a fresh local cache so daemon carries the full load.
            daemon_cache = os.path.join(workdir, "daemon-cache")
            os.makedirs(daemon_cache, exist_ok=True)
            # Pre-populate local cache once so hashes exist on disk.
            compile_with_trace(args.clang, source_path, daemon_cache,
                               Path(workdir) / "daemon-cold.json", args.std)
            for i in range(args.runs):
                trace = Path(workdir) / f"daemon_warm_{i}.json"
                stats = compile_with_trace(
                    args.clang, source_path, daemon_cache, trace, args.std,
                    daemon_url=args.daemon_url)
                daemon_times.append(stats["total_instantiate_ms"])
                daemon_hit_rates.append(stats["hit_rate"])
                daemon_hits_per_run.append(stats["daemon_hits"])
                daemon_shares.append(stats["daemon_share"])
                print(f"  Run {i+1}/{args.runs}: "
                      f"{format_ms(stats['total_instantiate_ms'])} "
                      f"(hits: {stats['cache_hits']}, "
                      f"daemon: {stats['daemon_hits']}, "
                      f"share: {stats['daemon_share']:.1f}%)")

        print()
        print("Results")
        print("=" * 60)

        cold_total = cold_stats["total_instantiate_ms"]
        warm_mean = mean(warm_times) if warm_times else 0
        warm_med = median(warm_times) if warm_times else 0
        warm_std = stdev(warm_times) if len(warm_times) > 1 else 0

        speedup_mean = cold_total / warm_mean if warm_mean > 0 else float("inf")
        speedup_med = cold_total / warm_med if warm_med > 0 else float("inf")
        time_saved_mean = cold_total - warm_mean
        pct_saved = (time_saved_mean / cold_total * 100) if cold_total > 0 else 0

        # Three-row table: cold / local warm / daemon warm
        col_w = 18
        print(f"{'Mode':<20} {'Inst. time':>{col_w}} {'Speedup':>{col_w}} "
              f"{'Hit rate':>{col_w}} {'Time saved':>{col_w}}")
        print("-" * (20 + col_w * 4 + 3))

        def row(label, t_ms, speedup, hit_rate, saved_ms):
            pct = f"{saved_ms/cold_total*100:.0f}%" if cold_total > 0 else "—"
            print(f"{label:<20} {format_ms(t_ms):>{col_w}} "
                  f"{speedup:>{col_w}.1f}x "
                  f"{hit_rate:>{col_w}.1f}% "
                  f"{format_ms(saved_ms) + ' (' + pct + ')':>{col_w}}")

        row("Cold (miss)", cold_total, 1.0, 0.0, 0.0)
        row("Warm local disk", warm_mean, speedup_mean,
            mean(warm_hit_rates) if warm_hit_rates else 0, time_saved_mean)

        if daemon_times:
            d_mean = mean(daemon_times)
            d_speedup = cold_total / d_mean if d_mean > 0 else float("inf")
            d_saved = cold_total - d_mean
            d_hit_rate = mean(daemon_hit_rates) if daemon_hit_rates else 0
            row("Warm daemon", d_mean, d_speedup, d_hit_rate, d_saved)

            avg_daemon_hits = mean(daemon_hits_per_run) if daemon_hits_per_run else 0
            avg_share = mean(daemon_shares) if daemon_shares else 0
            print()
            print(f"Daemon details:")
            print(f"  Avg daemon hits/run: {avg_daemon_hits:.0f}")
            print(f"  Avg daemon share:    {avg_share:.1f}% of hits from daemon")
            marginal_ms = warm_mean - d_mean
            if marginal_ms > 0:
                print(f"  Marginal speedup vs local disk: "
                      f"{format_ms(marginal_ms)} faster per compilation")

        print()
        print(f"Local disk hit rate:  {mean(warm_hit_rates):.1f}% avg, "
              f"{mean(warm_hits):.0f} hits/run avg")

        # Return non-zero if speedup is unreasonably small (< 1.5x).
        if speedup_mean < 1.5 and cold_total > 50:
            print("\nWARNING: Speedup < 1.5x — cache may not be working "
                  "correctly.", file=sys.stderr)
            return 1

    finally:
        if cleanup_workdir:
            shutil.rmtree(workdir, ignore_errors=True)
        if cleanup_cache and cache_dir != args.cache_dir:
            shutil.rmtree(cache_dir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
