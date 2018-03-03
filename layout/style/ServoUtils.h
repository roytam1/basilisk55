/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* some utilities for stylo */

#ifndef mozilla_ServoUtils_h
#define mozilla_ServoUtils_h

#include "mozilla/TypeTraits.h"

#ifdef MOZ_STYLO
# define MOZ_DECL_STYLO_CHECK_METHODS \
  bool IsGoanna() const { return !IsServo(); } \
  bool IsServo() const { return mType == StyleBackendType::Servo; }
#else
# define MOZ_DECL_STYLO_CHECK_METHODS \
  bool IsGoanna() const { return true; } \
  bool IsServo() const { return false; }
#endif

/**
 * Macro used in a base class of |goannatype_| and |servotype_|.
 * The class should define |StyleBackendType mType;| itself.
 */
#define MOZ_DECL_STYLO_METHODS(goannatype_, servotype_)  \
  MOZ_DECL_STYLO_CHECK_METHODS                          \
  inline goannatype_* AsGoanna();                         \
  inline servotype_* AsServo();                         \
  inline const goannatype_* AsGoanna() const;             \
  inline const servotype_* AsServo() const;

/**
 * Macro used in inline header of class |type_| with its Goanna and Servo
 * subclasses named |goannatype_| and |servotype_| correspondingly for
 * implementing the inline methods defined by MOZ_DECL_STYLO_METHODS.
 */
#define MOZ_DEFINE_STYLO_METHODS(type_, goannatype_, servotype_) \
  goannatype_* type_::AsGoanna() {                                \
    MOZ_ASSERT(IsGoanna());                                      \
    return static_cast<goannatype_*>(this);                      \
  }                                                             \
  servotype_* type_::AsServo() {                                \
    MOZ_ASSERT(IsServo());                                      \
    return static_cast<servotype_*>(this);                      \
  }                                                             \
  const goannatype_* type_::AsGoanna() const {                    \
    MOZ_ASSERT(IsGoanna());                                      \
    return static_cast<const goannatype_*>(this);                \
  }                                                             \
  const servotype_* type_::AsServo() const {                    \
    MOZ_ASSERT(IsServo());                                      \
    return static_cast<const servotype_*>(this);                \
  }

#define MOZ_STYLO_THIS_TYPE  mozilla::RemovePointer<decltype(this)>::Type
#define MOZ_STYLO_GOANNA_TYPE mozilla::RemovePointer<decltype(AsGoanna())>::Type
#define MOZ_STYLO_SERVO_TYPE mozilla::RemovePointer<decltype(AsServo())>::Type

/**
 * Macro used to forward a method call to the concrete method defined by
 * the Servo or Goanna implementation. The class of the method using it
 * should use MOZ_DECL_STYLO_METHODS to define basic stylo methods.
 */
#define MOZ_STYLO_FORWARD_CONCRETE(method_, goannaargs_, servoargs_)         \
  static_assert(!mozilla::IsSame<decltype(&MOZ_STYLO_THIS_TYPE::method_),   \
                                 decltype(&MOZ_STYLO_GOANNA_TYPE::method_)>  \
                ::value, "Goanna subclass should define its own " #method_); \
  static_assert(!mozilla::IsSame<decltype(&MOZ_STYLO_THIS_TYPE::method_),   \
                                 decltype(&MOZ_STYLO_SERVO_TYPE::method_)>  \
                ::value, "Servo subclass should define its own " #method_); \
  if (IsServo()) {                                                          \
    return AsServo()->method_ servoargs_;                                   \
  }                                                                         \
  return AsGoanna()->method_ goannaargs_;

#define MOZ_STYLO_FORWARD(method_, args_) \
  MOZ_STYLO_FORWARD_CONCRETE(method_, args_, args_)

#endif // mozilla_ServoUtils_h
