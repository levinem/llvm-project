// Test that structural equivalence mismatch (ODR-like conflict) between a
// cached specialization and the consuming TU's version is handled safely.
// The cache should detect the mismatch and fall back to normal instantiation
// rather than silently providing a wrong result.
//
// This test is structured so that the second compilation's template definition
// is subtly different from the first (a different field name), which would be
// an ODR violation in real code. The cache must not corrupt the AST.
//
// RUN: rm -rf %t.cache %t.dir
// RUN: mkdir -p %t.dir
// RUN: split-file %s %t.dir
//
// Populate cache with V1.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %t.dir/v1.cpp 2>&1 | FileCheck %s --check-prefix=V1
//
// Compile V2 (different struct layout) — should not crash even if it hits the
// V1 cache entry and detects a mismatch.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %t.dir/v2.cpp 2>&1 | FileCheck %s --check-prefix=V2

//--- v1.cpp
template <typename T>
struct Tagged {
  T data;         // field named 'data'
  int tag = 0;
  T get() const { return data; }
};

void use_v1() {
  Tagged<int> t;
  t.data = 42;
  (void)t.get();
}

//--- v2.cpp
// Same template name, different field name — structural mismatch.
template <typename T>
struct Tagged {
  T payload;      // field named 'payload' — ODR violation vs v1
  int tag = 0;
  T get() const { return payload; }
};

void use_v2() {
  Tagged<int> t;
  t.payload = 42;
  (void)t.get();
}

// V1: Cache writes:  {{[1-9]}}
// V1: Cache errors:  0

// V2 must not crash. It either hits the cache (if hash matches despite diff)
// or misses. Either way, compilation must succeed.
// V2-NOT: LLVM ERROR
// V2-NOT: Assertion
// V2: Cache errors:  0
