// Test that filesystem errors are handled gracefully.
// The cache should never cause compilation to fail — errors degrade to cache misses.
//
// Test 1: Cache directory doesn't exist and can't be created (bad path).
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=/nonexistent/deeply/nested/path/that/cannot/be/created \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=BAD-PATH
//
// Test 2: Cache index exists but is a directory (not a file).
// RUN: rm -rf %t.cache2
// RUN: mkdir -p %t.cache2
// RUN: mkdir -p %t.cache2/index.tic
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache2 \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=DIR-AS-FILE
//
// Test 3: Cache directory is read-only (cannot write new entries).
// Note: Skip on systems where root can write anywhere.
// REQUIRES: !root
// RUN: rm -rf %t.cache3
// RUN: mkdir -p %t.cache3
// RUN: chmod 555 %t.cache3
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache3 \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=READ-ONLY
// RUN: chmod 755 %t.cache3

template <typename T>
struct Safe {
  T value;
  explicit Safe(T v) : value(v) {}
  T get() const { return value; }
};

void use() {
  Safe<int> s(1);
  (void)s.get();
}

// BAD-PATH: compilation succeeded (or just stats line, no crash)
// BAD-PATH-NOT: error:
// BAD-PATH: Cache hits: 0

// DIR-AS-FILE-NOT: error:
// DIR-AS-FILE: Cache hits: 0

// READ-ONLY-NOT: error:
// READ-ONLY: Cache hits: 0
