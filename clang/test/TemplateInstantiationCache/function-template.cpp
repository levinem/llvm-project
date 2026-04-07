// Test caching of function template specializations.
//
// RUN: rm -rf %t.cache
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=WRITE
//
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=READ

template <typename T>
T clamp(T val, T lo, T hi) {
  if (val < lo) return lo;
  if (val > hi) return hi;
  return val;
}

template <typename T>
T square(T x) { return x * x; }

void use_functions() {
  (void)clamp(5, 1, 10);
  (void)clamp(3.14, 0.0, 1.0);
  (void)square(7);
  (void)square(2.5);
}

// WRITE: Cache writes: {{[1-9]}}
// READ:  Cache hits:   {{[1-9]}}
