// Test that the template instantiation cache works correctly when templates
// come from C++20 named modules. Module-resident templates should still
// benefit from the cache when instantiated in consumer TUs.
//
// split-file is used to put the module and consumer in one test file.
//
// RUN: rm -rf %t.cache %t.dir
// RUN: mkdir -p %t.dir
// RUN: split-file %s %t.dir
//
// Step 1: Build the module interface.
// RUN: %clang_cc1 -std=c++20 \
// RUN:   %t.dir/MyMath.cppm -emit-module-interface \
// RUN:   -o %t.dir/MyMath.pcm
//
// Step 2: First consumer compilation — populates cache.
// RUN: %clang_cc1 -std=c++20 \
// RUN:   -fprebuilt-module-path=%t.dir \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   -fsyntax-only %t.dir/Consumer.cpp 2>&1 | FileCheck %s --check-prefix=FIRST
//
// Step 3: Second consumer compilation — should hit cache.
// RUN: %clang_cc1 -std=c++20 \
// RUN:   -fprebuilt-module-path=%t.dir \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   -fsyntax-only %t.dir/Consumer.cpp 2>&1 | FileCheck %s --check-prefix=SECOND

//--- MyMath.cppm
export module MyMath;

export template <typename T>
struct Point {
  T x, y;
  Point(T a, T b) : x(a), y(b) {}
  T length_sq() const { return x*x + y*y; }
};

export template <typename T>
T dot(Point<T> a, Point<T> b) {
  return a.x * b.x + a.y * b.y;
}

//--- Consumer.cpp
import MyMath;

void use_point() {
  Point<float> p(3.0f, 4.0f);
  (void)p.length_sq();
  Point<float> q(1.0f, 0.0f);
  (void)dot(p, q);
}

// FIRST: Cache errors:    0

// SECOND: Cache hits:     {{[1-9]}}
// SECOND: Cache errors:   0
