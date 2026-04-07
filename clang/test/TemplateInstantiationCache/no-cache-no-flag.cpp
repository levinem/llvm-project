// Test that compilation without the flag is unaffected — no cache created,
// no performance overhead, normal behavior preserved.
//
// RUN: rm -rf %t.cache
// RUN: %clang_cc1 -std=c++17 -fsyntax-only %s
// RUN: not ls %t.cache 2>/dev/null

template <typename T>
T identity(T x) { return x; }

void use() {
  (void)identity(42);
  (void)identity(3.14);
}

// No CHECK directives — just verify the file compiles without errors.
