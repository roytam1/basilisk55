/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi.h"
#include "jswrapper.h"

#include "jsatominlines.h"

using namespace js;

template <class Base>
bool
SecurityWrapper<Base>::enter(JSContext* cx, HandleObject wrapper, HandleId id,
                             Wrapper::Action act, bool mayThrow, bool* bp) const
{
    ReportAccessDenied(cx);
    *bp = false;
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                                  const CallArgs& args) const
{
    ReportAccessDenied(cx);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::setPrototype(JSContext* cx, HandleObject wrapper, HandleObject proto,
                                    ObjectOpResult& result) const
{
    ReportAccessDenied(cx);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::setImmutablePrototype(JSContext* cx, HandleObject wrapper,
                                             bool* succeeded) const
{
    ReportAccessDenied(cx);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::preventExtensions(JSContext* cx, HandleObject wrapper,
                                         ObjectOpResult& result) const
{
    // Just like BaseProxyHandler, SecurityWrappers claim by default to always
    // be extensible, so as not to leak information about the state of the
    // underlying wrapped thing.
    return result.fail(JSMSG_CANT_CHANGE_EXTENSIBILITY);
}

template <class Base>
bool
SecurityWrapper<Base>::isExtensible(JSContext* cx, HandleObject wrapper, bool* extensible) const
{
    // See above.
    *extensible = true;
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::getBuiltinClass(JSContext* cx, HandleObject wrapper,
                                       ESClass* cls) const
{
    *cls = ESClass::Other;
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::isArray(JSContext* cx, HandleObject obj, JS::IsArrayAnswer* answer) const
{
    // This should ReportAccessDenied(cx), but bug 849730 disagrees.  :-(
    *answer = JS::IsArrayAnswer::NotArray;
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::regexp_toShared(JSContext* cx, HandleObject obj,
                                       MutableHandle<RegExpShared*> shared) const
{
    return Base::regexp_toShared(cx, obj, shared);
}

template <class Base>
bool
SecurityWrapper<Base>::boxedValue_unbox(JSContext* cx, HandleObject obj, MutableHandleValue vp) const
{
    vp.setUndefined();
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                      Handle<PropertyDescriptor> desc,
                                      ObjectOpResult& result) const
{
    if (desc.getter() || desc.setter()) {
        RootedValue idVal(cx, IdToValue(id));
        JSString* str = ValueToSource(cx, idVal);
        if (!str)
            return false;
        AutoStableStringChars chars(cx);
        const char16_t* prop = nullptr;
        if (str->ensureFlat(cx) && chars.initTwoByte(cx, str))
            prop = chars.twoByteChars();
        JS_ReportErrorNumberUC(cx, GetErrorMessage, nullptr,
                               JSMSG_ACCESSOR_DEF_DENIED, prop);
        return false;
    }

    return Base::defineProperty(cx, wrapper, id, desc, result);
}

template class js::SecurityWrapper<Wrapper>;
template class js::SecurityWrapper<CrossCompartmentWrapper>;
