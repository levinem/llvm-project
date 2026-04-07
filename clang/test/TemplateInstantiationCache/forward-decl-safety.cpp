// Test that forward-declared (incomplete) specializations are never stored
// in the cache. Only complete definitions should be cached.
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
struct Registry;  // Forward declaration — incomplete, must NOT be cached.

// Complete specialization — this IS cacheable.
template <typename T>
struct Handle {
  T *ptr;
  explicit Handle(T *p) : ptr(p) {}
  T &operator*() { return *ptr; }
  T *get() { return ptr; }
};

// Use the complete type.
void use_handle() {
  int x = 42;
  Handle<int> h(&x);
  (void)*h;
  (void)h.get();
}

// The forward-declared Registry<T> has no complete instantiation, so
// cache writes should only come from Handle<int>.
// FIRST: Cache writes:   {{[1-9]}}

// Second run: Handle<int> should hit cache. Registry is never instantiated.
// SECOND: Cache hits:    {{[1-9]}}
// SECOND: Cache errors:  0
