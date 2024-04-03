/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ServoDeclarationBlock.h"

#include "mozilla/ServoBindings.h"

#include "nsCSSProps.h"

namespace mozilla {

/* static */ already_AddRefed<ServoDeclarationBlock>
ServoDeclarationBlock::FromCssText(const nsAString& aCssText)
{
  return nullptr;
}

void
ServoDeclarationBlock::GetPropertyValue(const nsAString& aProperty,
                                        nsAString& aValue) const
{
}

void
ServoDeclarationBlock::GetPropertyValueByID(nsCSSPropertyID aPropID,
                                            nsAString& aValue) const
{
}

bool
ServoDeclarationBlock::GetPropertyIsImportant(const nsAString& aProperty) const
{
  return false;
}

void
ServoDeclarationBlock::RemoveProperty(const nsAString& aProperty)
{
}

void
ServoDeclarationBlock::RemovePropertyByID(nsCSSPropertyID aPropID)
{
}

} // namespace mozilla
