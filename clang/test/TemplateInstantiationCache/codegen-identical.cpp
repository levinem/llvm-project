// Test that codegen output is identical with and without cache.
// This regression test ensures the cache doesn't alter observable behavior.
//
// RUN: rm -rf %t.cache %t.no-cache.ll %t.with-cache-1.ll %t.with-cache-2.ll
//
// Baseline: compile without cache, emit IR.
// RUN: %clang_cc1 -std=c++17 -emit-llvm -o %t.no-cache.ll %s
//
// First compilation with cache (writes to cache).
// RUN: %clang_cc1 -std=c++17 -emit-llvm \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -o %t.with-cache-1.ll %s
//
// Second compilation with cache (reads from cache).
// RUN: %clang_cc1 -std=c++17 -emit-llvm \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -o %t.with-cache-2.ll %s
//
// All three outputs must be identical.
// RUN: diff %t.no-cache.ll %t.with-cache-1.ll
// RUN: diff %t.no-cache.ll %t.with-cache-2.ll

template <typename T>
struct Vec3 {
  T x, y, z;
  Vec3(T a, T b, T c) : x(a), y(b), z(c) {}
  T dot(const Vec3 &other) const {
    return x * other.x + y * other.y + z * other.z;
  }
  Vec3 operator+(const Vec3 &other) const {
    return Vec3(x + other.x, y + other.y, z + other.z);
  }
};

Vec3<float> add_vecs(Vec3<float> a, Vec3<float> b) {
  return a + b;
}

float dot_vecs(Vec3<float> a, Vec3<float> b) {
  return a.dot(b);
}
