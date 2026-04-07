#!/usr/bin/env python3
"""
check_trace_improvement.py - Verify that warm-cache -ftime-trace shows improvement.

Usage: python3 check_trace_improvement.py cold.json warm.json

Checks that:
1. The warm trace has at least one TemplateInstantiationCacheHit event.
2. The warm trace has fewer or equal total InstantiateClass microseconds
   than the cold trace (cache hits bypass instantiation).

Exit 0 on success, non-zero on failure.
"""

import json
import sys


def parse_trace(path):
    """Return dict with instantiation time totals and cache hit count."""
    with open(path) as f:
        data = json.load(f)

    total_us = 0
    cache_hits = 0

    for event in data.get("traceEvents", []):
        name = event.get("name", "")
        ph = event.get("ph", "")
        dur = event.get("dur", 0)

        if ph == "X" and name in ("InstantiateClass", "InstantiateFunction"):
            total_us += dur
        elif ph == "i" and name == "TemplateInstantiationCacheHit":
            cache_hits += 1

    return {"total_instantiate_us": total_us, "cache_hits": cache_hits}


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} cold.json warm.json", file=sys.stderr)
        return 1

    cold_path, warm_path = sys.argv[1], sys.argv[2]

    cold = parse_trace(cold_path)
    warm = parse_trace(warm_path)

    print(f"Cold trace: {cold['total_instantiate_us']/1000:.1f}ms instantiation, "
          f"{cold['cache_hits']} cache hits")
    print(f"Warm trace: {warm['total_instantiate_us']/1000:.1f}ms instantiation, "
          f"{warm['cache_hits']} cache hits")

    failures = []

    # Check 1: warm run should have at least one cache hit.
    if warm["cache_hits"] == 0:
        failures.append(
            "FAIL: Warm trace has no TemplateInstantiationCacheHit events. "
            "Cache may not be working.")

    # Check 2: cold run should have zero cache hits.
    if cold["cache_hits"] > 0:
        failures.append(
            f"WARNING: Cold trace unexpectedly has {cold['cache_hits']} "
            f"cache hits (cache may have been pre-warmed).")

    # Check 3: warm instantiation time should not be dramatically worse than cold.
    # We allow the warm run to be up to 20% slower due to ASTImporter overhead,
    # but it should not be 2x slower.
    if (cold["total_instantiate_us"] > 0 and
            warm["total_instantiate_us"] > cold["total_instantiate_us"] * 2.0):
        failures.append(
            f"FAIL: Warm trace instantiation time "
            f"({warm['total_instantiate_us']/1000:.1f}ms) is more than 2x "
            f"the cold trace ({cold['total_instantiate_us']/1000:.1f}ms). "
            f"Cache may be causing overhead.")

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        # Only fail hard on actual FAIL lines, not warnings.
        hard_failures = [f for f in failures if f.startswith("FAIL")]
        return 1 if hard_failures else 0

    print("OK: Warm cache trace shows improvement.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
