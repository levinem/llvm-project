// Test delta compression: multiple specializations of the same template
// should result in delta-compressed cache entries (smaller total size).
//
// RUN: rm -rf %t.cache
// RUN: %clang_cc1 -std=c++17 -fsyntax-only \
// RUN:   -ftemplate-instantiation-cache=%t.cache \
// RUN:   -ftemplate-instantiation-cache-stats \
// RUN:   %s 2>&1 | FileCheck %s

template <typename T>
struct Stack {
  T data[64];
  int top = 0;
  void push(T val) { data[top++] = val; }
  T pop() { return data[--top]; }
  bool empty() const { return top == 0; }
};

void use_stacks() {
  Stack<int>    si;    si.push(1);    (void)si.pop();
  Stack<double> sd;   sd.push(1.0);  (void)sd.pop();
  Stack<float>  sf;   sf.push(1.0f); (void)sf.pop();
  Stack<char>   sc;   sc.push('a');  (void)sc.pop();
  Stack<long>   sl;   sl.push(1L);   (void)sl.pop();
}

// CHECK: Cache writes: {{[1-9]}}
// At least some should be delta-compressed since they share the same template.
// CHECK: Delta compressions: {{[1-9]}}
