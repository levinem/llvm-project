// Test that -ftime-trace records cache hit events.
//
// REQUIRES: x86-registered-target
//
// RUN: rm -rf %t.cache %t.trace1.json %t.trace2.json
//
// First compilation: populates cache, trace shows no cache hit events.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftime-trace=%t.trace1.json \
// RUN:   -ftime-trace-granularity=0 \
// RUN:   %s
//
// Second compilation: loads from cache, trace should have cache hit events.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftime-trace=%t.trace2.json \
// RUN:   -ftime-trace-granularity=0 \
// RUN:   %s
//
// Verify second trace contains cache hit events.
// RUN: FileCheck %s --input-file=%t.trace2.json --check-prefix=TRACE

template <typename T>
struct Accumulator {
  T total = T();
  void add(T val) { total += val; }
  T get() const { return total; }
};

void use() {
  Accumulator<int> ai;
  ai.add(1);
  ai.add(2);
  (void)ai.get();
}

// TRACE: "TemplateInstantiationCacheHit"
