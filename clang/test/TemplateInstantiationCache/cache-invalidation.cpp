// Test that modifying the template definition invalidates the cache
// (the source content hash changes, producing a different cache key).
//
// This test verifies that after writing a cache entry for template V1,
// compiling a modified template V2 results in a cache miss (not a hit
// that would incorrectly reuse the V1 entry).
//
// RUN: rm -rf %t.cache
// RUN: mkdir -p %t.dir
//
// Write V1 of the template header.
// RUN: echo 'template<typename T> struct Widget { T val; T get() { return val; } };' \
// RUN:   > %t.dir/widget.h
//
// TU1: compile with V1, populate cache.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   -include %t.dir/widget.h %s 2>&1 | FileCheck %s --check-prefix=V1WRITE
//
// Write V2 of the template (added a new method).
// RUN: echo 'template<typename T> struct Widget { T val; T get() { return val; } T doubled() { return val + val; } };' \
// RUN:   > %t.dir/widget.h
//
// TU2: compile with V2, should get cache miss (different source hash).
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   -include %t.dir/widget.h %s 2>&1 | FileCheck %s --check-prefix=V2MISS

void use_widget() {
  Widget<int> w;
  w.val = 5;
  (void)w.get();
}

// V1WRITE: Cache writes: {{[1-9]}}
// V1WRITE: Cache hits:   0

// V2MISS: Cache hits:    0
// V2MISS: Cache writes:  {{[1-9]}}
