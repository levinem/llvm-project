// Test that explicit instantiation declarations (extern template) and
// explicit instantiation definitions (template) interact correctly with
// the cache. Explicit instantiation definitions should be cached;
// declarations (which suppress instantiation) should not produce cache entries.
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
struct Serializer {
  void write(const T &val);
  void read(T &val);
};

// Explicit instantiation declaration — suppresses implicit instantiation.
extern template struct Serializer<int>;

// Explicit instantiation definition — forces instantiation, must be cached.
template struct Serializer<double>;

void use() {
  Serializer<double> sd;
  // Serializer<int> is declared extern, so not instantiated here.
}

// FIRST: Cache writes: {{[1-9]}}
// SECOND: Cache hits:  {{[1-9]}}
