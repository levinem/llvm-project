// Test that the AST records how C-style and functional-style casts were
// resolved, and that this survives serialization.

// RUN: %clang_cc1 -triple x86_64-unknown-unknown -std=c++17 -ast-dump %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-unknown -std=c++17 -ast-dump=json %s \
// RUN:   | FileCheck %s --check-prefix=JSON
// RUN: %clang_cc1 -triple x86_64-unknown-unknown -std=c++17 -emit-pch -o %t %s
// RUN: %clang_cc1 -triple x86_64-unknown-unknown -std=c++17 -x c++ \
// RUN:   -include-pch %t -ast-dump-all /dev/null | FileCheck %s

struct B {
  virtual ~B();
};
struct D : B {};
using Long = long;

void casts(const int &cr, const int *cip, const B *cb, void *vp,
           const char *cc, int i, float f) {
  (void)(int &)cr;
  // CHECK: CStyleCastExpr {{.*}} 'int' lvalue <NoOp> resolved_as_const_cast casts_away_const{{$}}

  (void)(int *)cip;
  // CHECK: CStyleCastExpr {{.*}} 'int *' <NoOp> resolved_as_const_cast casts_away_const{{$}}

  (void)(const int *)cip;
  // CHECK: CStyleCastExpr {{.*}} 'const int *' <NoOp> resolved_as_const_cast{{$}}

  (void)(D *)cb;
  // CHECK: CStyleCastExpr {{.*}} 'D *' <BaseToDerived (B)> resolved_as_static_cast casts_away_const{{$}}

  (void)(int *)vp;
  // CHECK: CStyleCastExpr {{.*}} 'int *' <BitCast>{{$}}

  (void)(int *)cc;
  // CHECK: CStyleCastExpr {{.*}} 'int *' <BitCast> resolved_as_reinterpret_cast casts_away_const{{$}}

  (void)(long)&i;
  // CHECK: CStyleCastExpr {{.*}} 'long' <PointerToIntegral> resolved_as_reinterpret_cast{{$}}

  (void)(int)f;
  // CHECK: CStyleCastExpr {{.*}} 'int' <FloatingToIntegral>{{$}}

  (void)Long(&i);
  // CHECK: CXXFunctionalCastExpr {{.*}} functional cast to Long <PointerToIntegral> resolved_as_reinterpret_cast{{$}}
}

// JSON: "resolvedAs": "const_cast",
// JSON-NEXT: "castsAwayConst": true
// JSON: "resolvedAs": "reinterpret_cast",
// JSON-NEXT: "castsAwayConst": true
