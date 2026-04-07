// Test that a corrupt cache file is handled gracefully (treated as cache miss,
// no crash or assertion failure).
//
// RUN: rm -rf %t.cache
// RUN: mkdir -p %t.cache
//
// Write a corrupt index file.
// RUN: echo "GARBAGE_NOT_A_VALID_CACHE_FILE" > %t.cache/index.tic
//
// Compile should succeed despite corrupt cache (treats it as miss).
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s

template <typename T>
T double_val(T x) { return x + x; }

void use() {
  (void)double_val(5);
}

// Even with corrupt cache, compilation must succeed.
// Cache hits will be 0 (corruption = miss), but no errors that affect
// compilation.
// CHECK: Cache hits:   0
