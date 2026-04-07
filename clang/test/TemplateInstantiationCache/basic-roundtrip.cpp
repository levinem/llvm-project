// Test basic cache round-trip: TU1 instantiates and stores, TU2 loads from cache.
//
// RUN: rm -rf %t.cache
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=FIRST
//
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=SECOND

template <typename T>
struct Box {
  T value;
  T get() const { return value; }
  void set(T v) { value = v; }
};

void use_box() {
  Box<int> b;
  b.set(42);
  (void)b.get();

  Box<double> d;
  d.set(3.14);
  (void)d.get();
}

// FIRST: Template Instantiation Cache Statistics
// FIRST: Cache hits:      0
// FIRST: Cache writes:    {{[1-9]}}

// SECOND: Template Instantiation Cache Statistics
// SECOND: Cache hits:      {{[1-9]}}
