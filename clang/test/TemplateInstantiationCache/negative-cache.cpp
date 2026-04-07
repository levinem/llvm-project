// Test negative caching: SFINAE failures are cached and not re-evaluated.
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

#include <type_traits>

// is_addable checks if T + T is valid — will fail for non-numeric types.
template <typename T, typename = void>
struct is_addable : std::false_type {};

template <typename T>
struct is_addable<T, std::void_t<decltype(std::declval<T>() + std::declval<T>())>>
    : std::true_type {};

struct NonAddable {};

static_assert(!is_addable<NonAddable>::value, "NonAddable should not be addable");
static_assert(is_addable<int>::value, "int should be addable");

// WRITE: Cache writes: {{[1-9]}}
// WRITE: SFINAE cache hits: 0

// READ:  SFINAE cache hits: {{[1-9]}}
