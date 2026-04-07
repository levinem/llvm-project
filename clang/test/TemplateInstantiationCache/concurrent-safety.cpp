// Test that concurrent compilations don't corrupt the cache.
// Two compilations writing the same specialization must produce valid results.
//
// RUN: rm -rf %t.cache
//
// Run two compilations in parallel writing to the same cache.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache %s &
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache %s &
// RUN: wait
//
// A third compilation should be able to read from the (possibly partially
// written) cache without crashing. Errors are treated as cache misses.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s

template <typename T>
struct Counter {
  T count = 0;
  void increment() { ++count; }
  T get() const { return count; }
};

void use_counter() {
  Counter<int> c;
  c.increment();
  (void)c.get();
}

// The third compilation either hits or misses — both are correct.
// Just verify the cache infrastructure doesn't crash or error out.
// CHECK: Template Instantiation Cache Statistics
// CHECK-NOT: Cache errors: {{[1-9][0-9]*}}
