// Test caching with standard library-like types (testing complex template
// hierarchies that resemble real STL usage).
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

// Minimal allocator-like template hierarchy.
template <typename T>
struct Allocator {
  using value_type = T;
  T *allocate(unsigned n);
  void deallocate(T *p, unsigned n);
};

template <typename T, typename Alloc = Allocator<T>>
struct Vector {
  T *data_ = nullptr;
  unsigned size_ = 0;
  unsigned cap_ = 0;
  Alloc alloc_;

  void push_back(const T &val);
  T &operator[](unsigned i) { return data_[i]; }
  const T &operator[](unsigned i) const { return data_[i]; }
  unsigned size() const { return size_; }
  bool empty() const { return size_ == 0; }
};

template <typename K, typename V>
struct Map {
  struct Entry { K key; V value; };
  Vector<Entry> entries_;
  V *find(const K &key);
  void insert(const K &key, const V &value);
};

void use_stl_like() {
  Vector<int> vi;
  (void)vi.empty();

  Vector<double> vd;
  (void)vd.size();

  Map<int, int> m;
  (void)m.find(42);
}

// WRITE: Cache writes: {{[1-9]}}
// READ:  Cache hits:   {{[1-9]}}
