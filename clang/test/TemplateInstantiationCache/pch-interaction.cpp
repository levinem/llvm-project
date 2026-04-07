// Test that the template instantiation cache works correctly alongside PCH.
// When -fpch-instantiate-templates is used, templates are instantiated into
// the PCH. The cache should not duplicate this work or produce corrupt entries.
//
// RUN: rm -rf %t.cache %t.pch
//
// Step 1: Emit PCH (with template instantiation into PCH).
// RUN: %clang_cc1 -std=c++17 -emit-pch \
// RUN:   -fpch-instantiate-templates \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -x c++ -o %t.pch %s
//
// Step 2: Compile with the PCH (templates already in PCH, cache should not re-store).
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -include-pch %t.pch \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=WITH-PCH
//
// Step 3: Compile without PCH but with warm cache (should get hits).
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=WARM-CACHE

template <typename T>
struct Buffer {
  T storage[256];
  unsigned len = 0;
  void append(T val) { storage[len++] = val; }
  T at(unsigned i) const { return storage[i]; }
  unsigned size() const { return len; }
};

// Force instantiation.
template struct Buffer<int>;
template struct Buffer<double>;

void use_buffer() {
  Buffer<int> b;
  b.append(1);
  (void)b.at(0);
}

// WITH-PCH: Template Instantiation Cache Statistics
// WITH-PCH: Cache errors:    0

// WARM-CACHE: Cache hits:    {{[1-9]}}
// WARM-CACHE: Cache errors:  0
