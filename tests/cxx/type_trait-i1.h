//===--- type_trait-i1.h - test input file for iwyu -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef INCLUDE_WHAT_YOU_USE_TESTS_CXX_TYPE_TRAIT_I1_H_
#define INCLUDE_WHAT_YOU_USE_TESTS_CXX_TYPE_TRAIT_I1_H_

class Base {};

class Class {
 public:
  Class(Base*) noexcept;
  Class(Base&) noexcept;
  Class(void()) noexcept;
  Class(double, Base*) noexcept;

  Class& operator=(int) noexcept;
  Class& operator=(Base*) noexcept;
  Class& operator=(const Base&) noexcept;
  operator int() const noexcept;
  using FnRefType = void (&&)();
  operator FnRefType() const noexcept;

  int i, j;
};

union Union1;

struct Struct : Base {
  Struct(Union1*) noexcept;
  Struct(int, Base*, double) noexcept;
  Struct(void(), Union1*, Base&) noexcept;

  Struct& operator=(Class&) noexcept;
  Struct& operator=(Union1&) noexcept;
  Struct& operator=(Union1*) noexcept;
  operator Class&() const noexcept;
};

class StructDerivedClass : public Struct {};

union Union1 {
  Union1(const Base*) noexcept;
  Union1(int, const volatile Base*) noexcept;

  Union1& operator=(Struct&) noexcept;
  operator double() const noexcept;
};
union Union2 {
  Union2& operator=(Union1&) noexcept;
};

template <typename T>
struct DeducibleTpl {
  DeducibleTpl(T);
};

#endif  // INCLUDE_WHAT_YOU_USE_TESTS_CXX_TYPE_TRAIT_I1_H_
