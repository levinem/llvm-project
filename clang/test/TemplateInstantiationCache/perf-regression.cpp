// Performance regression test: verify that warm-cache compilation produces
// fewer template instantiation events than cold-cache compilation.
//
// This test uses -ftime-trace to count InstantiateClass events and asserts
// that the warm run has fewer (some hits that bypass instantiation entirely).
//
// REQUIRES: python3
// REQUIRES: shell
//
// RUN: rm -rf %t.cache %t.cold.json %t.warm.json
//
// Cold run: populate cache.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftime-trace=%t.cold.json \
// RUN:   -ftime-trace-granularity=0 \
// RUN:   %s
//
// Warm run: read from cache.
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftime-trace=%t.warm.json \
// RUN:   -ftime-trace-granularity=0 \
// RUN:   %s
//
// Python: verify warm trace has cache hit events and warm instantiation
// total is less than cold.
// RUN: python3 %S/check_trace_improvement.py %t.cold.json %t.warm.json

template <typename T>
struct Measured {
  T val;
  explicit Measured(T v) : val(v) {}
  T get() const { return val; }
  T doubled() const { return val + val; }
};

template <typename T>
Measured<T> make_measured(T v) {
  return Measured<T>(v);
}

void use() {
  auto a = make_measured(1);
  auto b = make_measured(2.0);
  auto c = make_measured(3.0f);
  (void)a.get();
  (void)b.doubled();
  (void)c.get();
}
