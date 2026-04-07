// Test caching of complex nested template specializations.
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
struct Optional {
  bool has_value = false;
  T value;
  Optional() = default;
  Optional(T v) : has_value(true), value(v) {}
  bool has() const { return has_value; }
  T get() const { return value; }
};

template <typename K, typename V>
struct Pair {
  K first;
  V second;
  Pair(K k, V v) : first(k), second(v) {}
};

// Nested: Optional<Pair<int, double>>
void use_nested() {
  Optional<int> oi(42);
  (void)oi.get();

  Optional<Pair<int, double>> op(Pair<int, double>(1, 2.0));
  (void)op.get().first;
}

// WRITE: Cache writes: {{[1-9]}}
// READ:  Cache hits:   {{[1-9]}}
