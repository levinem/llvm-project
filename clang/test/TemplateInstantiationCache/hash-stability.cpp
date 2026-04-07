// Test that the same specialization in two different TUs produces the same
// cache hash (round-trip succeeds).
//
// RUN: rm -rf %t.cache
//
// Compile TU1 (has the template + uses it)
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=TU1
//
// Compile TU2 (same template, same usage)
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=TU2

template <typename T>
struct Pair {
  T first;
  T second;
  Pair(T a, T b) : first(a), second(b) {}
  T sum() const { return first + second; }
};

void use_pair() {
  Pair<int> p(1, 2);
  (void)p.sum();
}

// TU1: Cache writes: {{[1-9]}}
// TU2: Cache hits:   {{[1-9]}}
// TU2: Cache misses: 0
