/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// We're dividing JS objects into 3 categories:
//
// 1. "real" roots, held by the JS engine itself or rooted through the root
//    and lock JS APIs. Roots from this category are considered black in the
//    cycle collector, any cycle they participate in is uncollectable.
//
// 2. certain roots held by C++ objects that are guaranteed to be alive.
//    Roots from this category are considered black in the cycle collector,
//    and any cycle they participate in is uncollectable. These roots are
//    traced from TraceNativeBlackRoots.
//
// 3. all other roots held by C++ objects that participate in cycle
//    collection, held by us (see TraceNativeGrayRoots). Roots from this
//    category are considered grey in the cycle collector; whether or not
//    they are collected depends on the objects that hold them.
//
// Note that if a root is in multiple categories the fact that it is in
// category 1 or 2 that takes precedence, so it will be considered black.
//
// During garbage collection we switch to an additional mark color (gray)
// when tracing inside TraceNativeGrayRoots. This allows us to walk those
// roots later on and add all objects reachable only from them to the
// cycle collector.
//
// Phases:
//
// 1. marking of the roots in category 1 by having the JS GC do its marking
// 2. marking of the roots in category 2 by having the JS GC call us back
//    (via JS_SetExtraGCRootsTracer) and running TraceNativeBlackRoots
// 3. marking of the roots in category 3 by TraceNativeGrayRoots using an
//    additional color (gray).
// 4. end of GC, GC can sweep its heap
//
// At some later point, when the cycle collector runs:
//
// 5. walk gray objects and add them to the cycle collector, cycle collect
//
// JS objects that are part of cycles the cycle collector breaks will be
// collected by the next JS GC.
//
// If WantAllTraces() is false the cycle collector will not traverse roots
// from category 1 or any JS objects held by them. Any JS objects they hold
// will already be marked by the JS GC and will thus be colored black
// themselves. Any C++ objects they hold will have a missing (untraversed)
// edge from the JS object to the C++ object and so it will be marked black
// too. This decreases the number of objects that the cycle collector has to
// deal with.
// To improve debugging, if WantAllTraces() is true all JS objects are
// traversed.

#include "mozilla/CycleCollectedJSContext.h"
#include <algorithm>
#include "mozilla/ArrayUtils.h"
#include "mozilla/AsyncEventDispatcher.h"
#include "mozilla/AutoRestore.h"
#include "mozilla/Move.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Telemetry.h"
#include "mozilla/TimelineConsumers.h"
#include "mozilla/TimelineMarker.h"
#include "mozilla/Unused.h"
#include "mozilla/DebuggerOnGCRunnable.h"
#include "mozilla/dom/DOMJSClass.h"
#include "mozilla/dom/ProfileTimelineMarkerBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseBinding.h"
#include "mozilla/dom/PromiseDebugging.h"
#include "mozilla/dom/PromiseRejectionEvent.h"
#include "mozilla/dom/PromiseRejectionEventBinding.h"
#include "mozilla/dom/ScriptSettings.h"
#include "jsprf.h"
#include "js/Debug.h"
#include "js/GCAPI.h"
#include "jsfriendapi.h"
#include "nsContentUtils.h"
#include "nsCycleCollectionNoteRootCallback.h"
#include "nsCycleCollectionParticipant.h"
#include "nsCycleCollector.h"
#include "nsDOMJSUtils.h"
#include "nsDOMMutationObserver.h"
#include "nsJSUtils.h"
#include "nsWrapperCache.h"
#include "nsStringBuffer.h"

#include "nsIException.h"
#include "nsIPlatformInfo.h"
#include "nsThread.h"
#include "nsThreadUtils.h"
#include "xpcpublic.h"

using namespace mozilla;
using namespace mozilla::dom;

namespace mozilla {

struct DeferredFinalizeFunctionHolder
{
  DeferredFinalizeFunction run;
  void* data;
};

class IncrementalFinalizeRunnable : public Runnable
{
  typedef AutoTArray<DeferredFinalizeFunctionHolder, 16> DeferredFinalizeArray;
  typedef CycleCollectedJSContext::DeferredFinalizerTable DeferredFinalizerTable;

  CycleCollectedJSContext* mContext;
  DeferredFinalizeArray mDeferredFinalizeFunctions;
  uint32_t mFinalizeFunctionToRun;
  bool mReleasing;

  static const PRTime SliceMillis = 5; /* ms */

public:
  IncrementalFinalizeRunnable(CycleCollectedJSContext* aCx,
                              DeferredFinalizerTable& aFinalizerTable);
  virtual ~IncrementalFinalizeRunnable();

  void ReleaseNow(bool aLimited);

  NS_DECL_NSIRUNNABLE
};

} // namespace mozilla

struct NoteWeakMapChildrenTracer : public JS::CallbackTracer
{
  NoteWeakMapChildrenTracer(JSContext* aCx,
                            nsCycleCollectionNoteRootCallback& aCb)
    : JS::CallbackTracer(aCx), mCb(aCb), mTracedAny(false), mMap(nullptr),
      mKey(nullptr), mKeyDelegate(nullptr)
  {
  }
  void onChild(const JS::GCCellPtr& aThing) override;
  nsCycleCollectionNoteRootCallback& mCb;
  bool mTracedAny;
  JSObject* mMap;
  JS::GCCellPtr mKey;
  JSObject* mKeyDelegate;
};

void
NoteWeakMapChildrenTracer::onChild(const JS::GCCellPtr& aThing)
{
  if (aThing.is<JSString>()) {
    return;
  }

  if (!JS::GCThingIsMarkedGray(aThing) && !mCb.WantAllTraces()) {
    return;
  }

  if (AddToCCKind(aThing.kind())) {
    mCb.NoteWeakMapping(mMap, mKey, mKeyDelegate, aThing);
    mTracedAny = true;
  } else {
    JS::TraceChildren(this, aThing);
  }
}

struct NoteWeakMapsTracer : public js::WeakMapTracer
{
  NoteWeakMapsTracer(JSContext* aCx, nsCycleCollectionNoteRootCallback& aCccb)
    : js::WeakMapTracer(aCx), mCb(aCccb), mChildTracer(aCx, aCccb)
  {
  }
  void trace(JSObject* aMap, JS::GCCellPtr aKey, JS::GCCellPtr aValue) override;
  nsCycleCollectionNoteRootCallback& mCb;
  NoteWeakMapChildrenTracer mChildTracer;
};

void
NoteWeakMapsTracer::trace(JSObject* aMap, JS::GCCellPtr aKey,
                          JS::GCCellPtr aValue)
{
  // If nothing that could be held alive by this entry is marked gray, return.
  if ((!aKey || !JS::GCThingIsMarkedGray(aKey)) &&
      MOZ_LIKELY(!mCb.WantAllTraces())) {
    if (!aValue || !JS::GCThingIsMarkedGray(aValue) || aValue.is<JSString>()) {
      return;
    }
  }

  // The cycle collector can only properly reason about weak maps if it can
  // reason about the liveness of their keys, which in turn requires that
  // the key can be represented in the cycle collector graph.  All existing
  // uses of weak maps use either objects or scripts as keys, which are okay.
  MOZ_ASSERT(AddToCCKind(aKey.kind()));

  // As an emergency fallback for non-debug builds, if the key is not
  // representable in the cycle collector graph, we treat it as marked.  This
  // can cause leaks, but is preferable to ignoring the binding, which could
  // cause the cycle collector to free live objects.
  if (!AddToCCKind(aKey.kind())) {
    aKey = nullptr;
  }

  JSObject* kdelegate = nullptr;
  if (aKey.is<JSObject>()) {
    kdelegate = js::GetWeakmapKeyDelegate(&aKey.as<JSObject>());
  }

  if (AddToCCKind(aValue.kind())) {
    mCb.NoteWeakMapping(aMap, aKey, kdelegate, aValue);
  } else {
    mChildTracer.mTracedAny = false;
    mChildTracer.mMap = aMap;
    mChildTracer.mKey = aKey;
    mChildTracer.mKeyDelegate = kdelegate;

    if (!aValue.is<JSString>()) {
      JS::TraceChildren(&mChildTracer, aValue);
    }

    // The delegate could hold alive the key, so report something to the CC
    // if we haven't already.
    if (!mChildTracer.mTracedAny &&
        aKey && JS::GCThingIsMarkedGray(aKey) && kdelegate) {
      mCb.NoteWeakMapping(aMap, aKey, kdelegate, nullptr);
    }
  }
}

// This is based on the logic in FixWeakMappingGrayBitsTracer::trace.
struct FixWeakMappingGrayBitsTracer : public js::WeakMapTracer
{
  explicit FixWeakMappingGrayBitsTracer(JSContext* aCx)
    : js::WeakMapTracer(aCx)
  {
  }

  void
  FixAll()
  {
    do {
      mAnyMarked = false;
      js::TraceWeakMaps(this);
    } while (mAnyMarked);
  }

  void trace(JSObject* aMap, JS::GCCellPtr aKey, JS::GCCellPtr aValue) override
  {
    // If nothing that could be held alive by this entry is marked gray, return.
    bool keyMightNeedMarking = aKey && JS::GCThingIsMarkedGray(aKey);
    bool valueMightNeedMarking = aValue && JS::GCThingIsMarkedGray(aValue) &&
                                 aValue.kind() != JS::TraceKind::String;
    if (!keyMightNeedMarking && !valueMightNeedMarking) {
      return;
    }

    if (!AddToCCKind(aKey.kind())) {
      aKey = nullptr;
    }

    if (keyMightNeedMarking && aKey.is<JSObject>()) {
      JSObject* kdelegate = js::GetWeakmapKeyDelegate(&aKey.as<JSObject>());
      if (kdelegate && !JS::ObjectIsMarkedGray(kdelegate) &&
          (!aMap || !JS::ObjectIsMarkedGray(aMap)))
      {
        if (JS::UnmarkGrayGCThingRecursively(aKey)) {
          mAnyMarked = true;
        }
      }
    }

    if (aValue && JS::GCThingIsMarkedGray(aValue) &&
        (!aKey || !JS::GCThingIsMarkedGray(aKey)) &&
        (!aMap || !JS::ObjectIsMarkedGray(aMap)) &&
        aValue.kind() != JS::TraceKind::Shape) {
      if (JS::UnmarkGrayGCThingRecursively(aValue)) {
        mAnyMarked = true;
      }
    }
  }

  MOZ_INIT_OUTSIDE_CTOR bool mAnyMarked;
};

static void
CheckParticipatesInCycleCollection(JS::GCCellPtr aThing, const char* aName,
                                   void* aClosure)
{
  bool* cycleCollectionEnabled = static_cast<bool*>(aClosure);

  if (*cycleCollectionEnabled) {
    return;
  }

  if (AddToCCKind(aThing.kind()) && JS::GCThingIsMarkedGray(aThing)) {
    *cycleCollectionEnabled = true;
  }
}

NS_IMETHODIMP
JSGCThingParticipant::TraverseNative(void* aPtr,
                                     nsCycleCollectionTraversalCallback& aCb)
{
  auto runtime = reinterpret_cast<CycleCollectedJSContext*>(
    reinterpret_cast<char*>(this) - offsetof(CycleCollectedJSContext,
                                             mGCThingCycleCollectorGlobal));

  JS::GCCellPtr cellPtr(aPtr, JS::GCThingTraceKind(aPtr));
  runtime->TraverseGCThing(CycleCollectedJSContext::TRAVERSE_FULL, cellPtr, aCb);
  return NS_OK;
}

// NB: This is only used to initialize the participant in
// CycleCollectedJSContext. It should never be used directly.
static JSGCThingParticipant sGCThingCycleCollectorGlobal;

NS_IMETHODIMP
JSZoneParticipant::TraverseNative(void* aPtr,
                                  nsCycleCollectionTraversalCallback& aCb)
{
  auto runtime = reinterpret_cast<CycleCollectedJSContext*>(
    reinterpret_cast<char*>(this) - offsetof(CycleCollectedJSContext,
                                             mJSZoneCycleCollectorGlobal));

  MOZ_ASSERT(!aCb.WantAllTraces());
  JS::Zone* zone = static_cast<JS::Zone*>(aPtr);

  runtime->TraverseZone(zone, aCb);
  return NS_OK;
}

struct TraversalTracer : public JS::CallbackTracer
{
  TraversalTracer(JSContext* aCx, nsCycleCollectionTraversalCallback& aCb)
    : JS::CallbackTracer(aCx, DoNotTraceWeakMaps), mCb(aCb)
  {
  }
  void onChild(const JS::GCCellPtr& aThing) override;
  nsCycleCollectionTraversalCallback& mCb;
};

void
TraversalTracer::onChild(const JS::GCCellPtr& aThing)
{
  // Don't traverse non-gray objects, unless we want all traces.
  if (!JS::GCThingIsMarkedGray(aThing) && !mCb.WantAllTraces()) {
    return;
  }

  /*
   * This function needs to be careful to avoid stack overflow. Normally, when
   * AddToCCKind is true, the recursion terminates immediately as we just add
   * |thing| to the CC graph. So overflow is only possible when there are long
   * or cyclic chains of non-AddToCCKind GC things. Places where this can occur
   * use special APIs to handle such chains iteratively.
   */
  if (AddToCCKind(aThing.kind())) {
    if (MOZ_UNLIKELY(mCb.WantDebugInfo())) {
      char buffer[200];
      getTracingEdgeName(buffer, sizeof(buffer));
      mCb.NoteNextEdgeName(buffer);
    }
    mCb.NoteJSChild(aThing);
  } else if (aThing.is<js::Shape>()) {
    // The maximum depth of traversal when tracing a Shape is unbounded, due to
    // the parent pointers on the shape.
    JS_TraceShapeCycleCollectorChildren(this, aThing);
  } else if (aThing.is<js::ObjectGroup>()) {
    // The maximum depth of traversal when tracing an ObjectGroup is unbounded,
    // due to information attached to the groups which can lead other groups to
    // be traced.
    JS_TraceObjectGroupCycleCollectorChildren(this, aThing);
  } else if (!aThing.is<JSString>()) {
    JS::TraceChildren(this, aThing);
  }
}

static void
NoteJSChildGrayWrapperShim(void* aData, JS::GCCellPtr aThing)
{
  TraversalTracer* trc = static_cast<TraversalTracer*>(aData);
  trc->onChild(aThing);
}

/*
 * The cycle collection participant for a Zone is intended to produce the same
 * results as if all of the gray GCthings in a zone were merged into a single node,
 * except for self-edges. This avoids the overhead of representing all of the GCthings in
 * the zone in the cycle collector graph, which should be much faster if many of
 * the GCthings in the zone are gray.
 *
 * Zone merging should not always be used, because it is a conservative
 * approximation of the true cycle collector graph that can incorrectly identify some
 * garbage objects as being live. For instance, consider two cycles that pass through a
 * zone, where one is garbage and the other is live. If we merge the entire
 * zone, the cycle collector will think that both are alive.
 *
 * We don't have to worry about losing track of a garbage cycle, because any such garbage
 * cycle incorrectly identified as live must contain at least one C++ to JS edge, and
 * XPConnect will always add the C++ object to the CC graph. (This is in contrast to pure
 * C++ garbage cycles, which must always be properly identified, because we clear the
 * purple buffer during every CC, which may contain the last reference to a garbage
 * cycle.)
 */

// NB: This is only used to initialize the participant in
// CycleCollectedJSContext. It should never be used directly.
static const JSZoneParticipant sJSZoneCycleCollectorGlobal;

static
void JSObjectsTenuredCb(JSContext* aContext, void* aData)
{
  static_cast<CycleCollectedJSContext*>(aData)->JSObjectsTenured();
}

bool
mozilla::GetBuildId(JS::BuildIdCharVector* aBuildID)
{
  nsCOMPtr<nsIPlatformInfo> info = do_GetService("@mozilla.org/xre/app-info;1");
  if (!info) {
    return false;
  }

  nsCString buildID;
  nsresult rv = info->GetPlatformBuildID(buildID);
  NS_ENSURE_SUCCESS(rv, false);

  if (!aBuildID->resize(buildID.Length())) {
    return false;
  }

  for (size_t i = 0; i < buildID.Length(); i++) {
    (*aBuildID)[i] = buildID[i];
  }

  return true;
}

CycleCollectedJSContext::CycleCollectedJSContext()
  : mGCThingCycleCollectorGlobal(sGCThingCycleCollectorGlobal)
  , mJSZoneCycleCollectorGlobal(sJSZoneCycleCollectorGlobal)
  , mJSContext(nullptr)
  , mPrevGCSliceCallback(nullptr)
  , mPrevGCNurseryCollectionCallback(nullptr)
  , mJSHolders(256)
  , mDoingStableStates(false)
  , mTargetedMicroTaskRecursionDepth(0)
  , mMicroTaskLevel(0)
  , mMicroTaskRecursionDepth(0)
  , mOutOfMemoryState(OOMState::OK)
  , mLargeAllocationFailureState(OOMState::OK)
{
  nsCOMPtr<nsIThread> thread = do_GetCurrentThread();
  mOwningThread = thread.forget().downcast<nsThread>().take();
  MOZ_RELEASE_ASSERT(mOwningThread);
}

CycleCollectedJSContext::~CycleCollectedJSContext()
{
  // If the allocation failed, here we are.
  if (!mJSContext) {
    return;
  }

  MOZ_ASSERT(!mDeferredFinalizerTable.Count());

  // Last chance to process any events.
  CleanupIDBTransactions(mBaseRecursionDepth);
  MOZ_ASSERT(mPendingIDBTransactions.IsEmpty());

  ProcessStableStateQueue();
  MOZ_ASSERT(mStableStateEvents.IsEmpty());

  // Clear mPendingException first, since it might be cycle collected.
  mPendingException = nullptr;

  MOZ_ASSERT(mDebuggerMicroTaskQueue.empty());
  MOZ_ASSERT(mPendingMicroTaskRunnables.empty());

  mUncaughtRejections.reset();
  mConsumedRejections.reset();

  JS_DestroyContext(mJSContext);
  mJSContext = nullptr;
  nsCycleCollector_forgetJSContext();

  mozilla::dom::DestroyScriptSettings();

  mOwningThread->SetScriptObserver(nullptr);
  NS_RELEASE(mOwningThread);
}

static void
MozCrashWarningReporter(JSContext*, JSErrorReport*)
{
  MOZ_CRASH("Why is someone touching JSAPI without an AutoJSAPI?");
}

nsresult
CycleCollectedJSContext::Initialize(JSContext* aParentContext,
                                    uint32_t aMaxBytes,
                                    uint32_t aMaxNurseryBytes)
{
  MOZ_ASSERT(!mJSContext);

  mOwningThread->SetScriptObserver(this);
  // The main thread has a base recursion depth of 0, workers of 1.
  mBaseRecursionDepth = RecursionDepth();

  mozilla::dom::InitScriptSettings();
  mJSContext = JS_NewContext(aMaxBytes, aMaxNurseryBytes, aParentContext);
  if (!mJSContext) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  NS_GetCurrentThread()->SetCanInvokeJS(true);

  if (!JS_AddExtraGCRootsTracer(mJSContext, TraceBlackJS, this)) {
    MOZ_CRASH("JS_AddExtraGCRootsTracer failed");
  }
  JS_SetGrayGCRootsTracer(mJSContext, TraceGrayJS, this);
  JS_SetGCCallback(mJSContext, GCCallback, this);
  mPrevGCSliceCallback = JS::SetGCSliceCallback(mJSContext, GCSliceCallback);

  if (NS_IsMainThread()) {
    // We would like to support all threads here, but the way timeline consumers
    // are set up currently, you can either add a marker for one specific
    // docshell, or for every consumer globally. We would like to add a marker
    // for every consumer observing anything on this thread, but that is not
    // currently possible. For now, add global markers only when we are on the
    // main thread, since the UI for this tracing data only displays data
    // relevant to the main-thread.
    mPrevGCNurseryCollectionCallback = JS::SetGCNurseryCollectionCallback(
      mJSContext, GCNurseryCollectionCallback);
  }

  JS_SetObjectsTenuredCallback(mJSContext, JSObjectsTenuredCb, this);
  JS::SetOutOfMemoryCallback(mJSContext, OutOfMemoryCallback, this);
  JS::SetLargeAllocationFailureCallback(mJSContext,
                                        LargeAllocationFailureCallback, this);
  JS_SetExternalStringSizeofCallback(mJSContext, SizeofExternalStringCallback);
  JS_SetDestroyZoneCallback(mJSContext, XPCStringConvert::FreeZoneCache);
  JS_SetSweepZoneCallback(mJSContext, XPCStringConvert::ClearZoneCache);
  JS::SetBuildIdOp(mJSContext, GetBuildId);
  JS::SetWarningReporter(mJSContext, MozCrashWarningReporter);

  static js::DOMCallbacks DOMcallbacks = {
    InstanceClassHasProtoAtDepth
  };
  SetDOMCallbacks(mJSContext, &DOMcallbacks);
  js::SetScriptEnvironmentPreparer(mJSContext, &mEnvironmentPreparer);

  JS::SetGetIncumbentGlobalCallback(mJSContext, GetIncumbentGlobalCallback);

  JS::SetEnqueuePromiseJobCallback(mJSContext, EnqueuePromiseJobCallback, this);
  JS::SetPromiseRejectionTrackerCallback(mJSContext, PromiseRejectionTrackerCallback, this);
  mUncaughtRejections.init(mJSContext, JS::GCVector<JSObject*, 0, js::SystemAllocPolicy>(js::SystemAllocPolicy()));
  mConsumedRejections.init(mJSContext, JS::GCVector<JSObject*, 0, js::SystemAllocPolicy>(js::SystemAllocPolicy()));

  JS::dbg::SetDebuggerMallocSizeOf(mJSContext, moz_malloc_size_of);

  nsCycleCollector_registerJSContext(this);

  return NS_OK;
}

size_t
CycleCollectedJSContext::SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const
{
  size_t n = 0;

  // We're deliberately not measuring anything hanging off the entries in
  // mJSHolders.
  n += mJSHolders.ShallowSizeOfExcludingThis(aMallocSizeOf);

  return n;
}

void
CycleCollectedJSContext::UnmarkSkippableJSHolders()
{
  for (auto iter = mJSHolders.Iter(); !iter.Done(); iter.Next()) {
    void* holder = iter.Key();
    nsScriptObjectTracer*& tracer = iter.Data();
    tracer->CanSkip(holder, true);
  }
}

void
CycleCollectedJSContext::DescribeGCThing(bool aIsMarked, JS::GCCellPtr aThing,
                                         nsCycleCollectionTraversalCallback& aCb) const
{
  if (!aCb.WantDebugInfo()) {
    aCb.DescribeGCedNode(aIsMarked, "JS Object");
    return;
  }

  char name[72];
  uint64_t compartmentAddress = 0;
  if (aThing.is<JSObject>()) {
    JSObject* obj = &aThing.as<JSObject>();
    compartmentAddress = (uint64_t)js::GetObjectCompartment(obj);
    const js::Class* clasp = js::GetObjectClass(obj);

    // Give the subclass a chance to do something
    if (DescribeCustomObjects(obj, clasp, name)) {
      // Nothing else to do!
    } else if (js::IsFunctionObject(obj)) {
      JSFunction* fun = JS_GetObjectFunction(obj);
      JSString* str = JS_GetFunctionDisplayId(fun);
      if (str) {
        JSFlatString* flat = JS_ASSERT_STRING_IS_FLAT(str);
        nsAutoString chars;
        AssignJSFlatString(chars, flat);
        NS_ConvertUTF16toUTF8 fname(chars);
        SprintfLiteral(name, "JS Object (Function - %s)", fname.get());
      } else {
        SprintfLiteral(name, "JS Object (Function)");
      }
    } else {
      SprintfLiteral(name, "JS Object (%s)", clasp->name);
    }
  } else {
    SprintfLiteral(name, "JS %s", JS::GCTraceKindToAscii(aThing.kind()));
  }

  // Disable printing global for objects while we figure out ObjShrink fallout.
  aCb.DescribeGCedNode(aIsMarked, name, compartmentAddress);
}

void
CycleCollectedJSContext::NoteGCThingJSChildren(JS::GCCellPtr aThing,
                                               nsCycleCollectionTraversalCallback& aCb) const
{
  MOZ_ASSERT(mJSContext);
  TraversalTracer trc(mJSContext, aCb);
  JS::TraceChildren(&trc, aThing);
}

void
CycleCollectedJSContext::NoteGCThingXPCOMChildren(const js::Class* aClasp,
                                                  JSObject* aObj,
                                                  nsCycleCollectionTraversalCallback& aCb) const
{
  MOZ_ASSERT(aClasp);
  MOZ_ASSERT(aClasp == js::GetObjectClass(aObj));

  if (NoteCustomGCThingXPCOMChildren(aClasp, aObj, aCb)) {
    // Nothing else to do!
    return;
  }
  // XXX This test does seem fragile, we should probably whitelist classes
  //     that do hold a strong reference, but that might not be possible.
  if (aClasp->flags & JSCLASS_HAS_PRIVATE &&
      aClasp->flags & JSCLASS_PRIVATE_IS_NSISUPPORTS) {
    NS_CYCLE_COLLECTION_NOTE_EDGE_NAME(aCb, "js::GetObjectPrivate(obj)");
    aCb.NoteXPCOMChild(static_cast<nsISupports*>(js::GetObjectPrivate(aObj)));
    return;
  }

  const DOMJSClass* domClass = GetDOMClass(aObj);
  if (domClass) {
    NS_CYCLE_COLLECTION_NOTE_EDGE_NAME(aCb, "UnwrapDOMObject(obj)");
    // It's possible that our object is an unforgeable holder object, in
    // which case it doesn't actually have a C++ DOM object associated with
    // it.  Use UnwrapPossiblyNotInitializedDOMObject, which produces null in
    // that case, since NoteXPCOMChild/NoteNativeChild are null-safe.
    if (domClass->mDOMObjectIsISupports) {
      aCb.NoteXPCOMChild(
          UnwrapPossiblyNotInitializedDOMObject<nsISupports>(aObj));
    } else if (domClass->mParticipant) {
      aCb.NoteNativeChild(UnwrapPossiblyNotInitializedDOMObject<void>(aObj),
                          domClass->mParticipant);
         }
    return;
  }

  JS::Value value = js::MaybeGetScriptPrivate(aObj);
  if (!value.isUndefined()) {
    aCb.NoteXPCOMChild(static_cast<nsISupports*>(value.toPrivate()));
  }
}

void
CycleCollectedJSContext::TraverseGCThing(TraverseSelect aTs, JS::GCCellPtr aThing,
                                         nsCycleCollectionTraversalCallback& aCb)
{
  bool isMarkedGray = JS::GCThingIsMarkedGray(aThing);

  if (aTs == TRAVERSE_FULL) {
    DescribeGCThing(!isMarkedGray, aThing, aCb);
  }

  // If this object is alive, then all of its children are alive. For JS objects,
  // the black-gray invariant ensures the children are also marked black. For C++
  // objects, the ref count from this object will keep them alive. Thus we don't
  // need to trace our children, unless we are debugging using WantAllTraces.
  if (!isMarkedGray && !aCb.WantAllTraces()) {
    return;
  }

  if (aTs == TRAVERSE_FULL) {
    NoteGCThingJSChildren(aThing, aCb);
  }

  if (aThing.is<JSObject>()) {
    JSObject* obj = &aThing.as<JSObject>();
    NoteGCThingXPCOMChildren(js::GetObjectClass(obj), obj, aCb);
  }
}

struct TraverseObjectShimClosure
{
  nsCycleCollectionTraversalCallback& cb;
  CycleCollectedJSContext* self;
};

void
CycleCollectedJSContext::TraverseZone(JS::Zone* aZone,
                                      nsCycleCollectionTraversalCallback& aCb)
{
  MOZ_ASSERT(mJSContext);

  /*
   * We treat the zone as being gray. We handle non-gray GCthings in the
   * zone by not reporting their children to the CC. The black-gray invariant
   * ensures that any JS children will also be non-gray, and thus don't need to be
   * added to the graph. For C++ children, not representing the edge from the
   * non-gray JS GCthings to the C++ object will keep the child alive.
   *
   * We don't allow zone merging in a WantAllTraces CC, because then these
   * assumptions don't hold.
   */
  aCb.DescribeGCedNode(false, "JS Zone");

  /*
   * Every JS child of everything in the zone is either in the zone
   * or is a cross-compartment wrapper. In the former case, we don't need to
   * represent these edges in the CC graph because JS objects are not ref counted.
   * In the latter case, the JS engine keeps a map of these wrappers, which we
   * iterate over. Edges between compartments in the same zone will add
   * unnecessary loop edges to the graph (bug 842137).
   */
  TraversalTracer trc(mJSContext, aCb);
  js::VisitGrayWrapperTargets(aZone, NoteJSChildGrayWrapperShim, &trc);

  /*
   * To find C++ children of things in the zone, we scan every JS Object in
   * the zone. Only JS Objects can have C++ children.
   */
  TraverseObjectShimClosure closure = { aCb, this };
  js::IterateGrayObjects(aZone, TraverseObjectShim, &closure);
}

/* static */ void
CycleCollectedJSContext::TraverseObjectShim(void* aData, JS::GCCellPtr aThing)
{
  TraverseObjectShimClosure* closure =
    static_cast<TraverseObjectShimClosure*>(aData);

  MOZ_ASSERT(aThing.is<JSObject>());
  closure->self->TraverseGCThing(CycleCollectedJSContext::TRAVERSE_CPP,
                                 aThing, closure->cb);
}

void
CycleCollectedJSContext::TraverseNativeRoots(nsCycleCollectionNoteRootCallback& aCb)
{
  // NB: This is here just to preserve the existing XPConnect order. I doubt it
  // would hurt to do this after the JS holders.
  TraverseAdditionalNativeRoots(aCb);

  for (auto iter = mJSHolders.Iter(); !iter.Done(); iter.Next()) {
    void* holder = iter.Key();
    nsScriptObjectTracer*& tracer = iter.Data();

    bool noteRoot = false;
    if (MOZ_UNLIKELY(aCb.WantAllTraces())) {
      noteRoot = true;
    } else {
      tracer->Trace(holder,
                    TraceCallbackFunc(CheckParticipatesInCycleCollection),
                    &noteRoot);
    }

    if (noteRoot) {
      aCb.NoteNativeRoot(holder, tracer);
    }
  }
}

/* static */ void
CycleCollectedJSContext::TraceBlackJS(JSTracer* aTracer, void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);

  self->TraceNativeBlackRoots(aTracer);
}

/* static */ void
CycleCollectedJSContext::TraceGrayJS(JSTracer* aTracer, void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);

  // Mark these roots as gray so the CC can walk them later.
  self->TraceNativeGrayRoots(aTracer);
}

/* static */ void
CycleCollectedJSContext::GCCallback(JSContext* aContext,
                                    JSGCStatus aStatus,
                                    void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);

  MOZ_ASSERT(aContext == self->Context());

  self->OnGC(aStatus);
}

/* static */ void
CycleCollectedJSContext::GCSliceCallback(JSContext* aContext,
                                         JS::GCProgress aProgress,
                                         const JS::GCDescription& aDesc)
{
  CycleCollectedJSContext* self = CycleCollectedJSContext::Get();
  MOZ_ASSERT(self->Context() == aContext);

  if (aProgress == JS::GC_CYCLE_END) {
    JS::gcreason::Reason reason = aDesc.reason_;
    Unused <<
      NS_WARN_IF(NS_FAILED(DebuggerOnGCRunnable::Enqueue(aContext, aDesc)) &&
                 reason != JS::gcreason::SHUTDOWN_CC &&
                 reason != JS::gcreason::DESTROY_RUNTIME &&
                 reason != JS::gcreason::XPCONNECT_SHUTDOWN);
  }

  if (self->mPrevGCSliceCallback) {
    self->mPrevGCSliceCallback(aContext, aProgress, aDesc);
  }
}

class MinorGCMarker : public TimelineMarker
{
private:
  JS::gcreason::Reason mReason;

public:
  MinorGCMarker(MarkerTracingType aTracingType,
                JS::gcreason::Reason aReason)
    : TimelineMarker("MinorGC",
                     aTracingType,
                     MarkerStackRequest::NO_STACK)
    , mReason(aReason)
    {
      MOZ_ASSERT(aTracingType == MarkerTracingType::START ||
                 aTracingType == MarkerTracingType::END);
    }

  MinorGCMarker(JS::GCNurseryProgress aProgress,
                JS::gcreason::Reason aReason)
    : TimelineMarker("MinorGC",
                     aProgress == JS::GCNurseryProgress::GC_NURSERY_COLLECTION_START
                       ? MarkerTracingType::START
                       : MarkerTracingType::END,
                     MarkerStackRequest::NO_STACK)
    , mReason(aReason)
  { }

  virtual void
  AddDetails(JSContext* aCx,
             dom::ProfileTimelineMarker& aMarker) override
  {
    TimelineMarker::AddDetails(aCx, aMarker);

    if (GetTracingType() == MarkerTracingType::START) {
      auto reason = JS::gcreason::ExplainReason(mReason);
      aMarker.mCauseName.Construct(NS_ConvertUTF8toUTF16(reason));
    }
  }

  virtual UniquePtr<AbstractTimelineMarker>
  Clone() override
  {
    auto clone = MakeUnique<MinorGCMarker>(GetTracingType(), mReason);
    clone->SetCustomTime(GetTime());
    return UniquePtr<AbstractTimelineMarker>(Move(clone));
  }
};

/* static */ void
CycleCollectedJSContext::GCNurseryCollectionCallback(JSContext* aContext,
                                                     JS::GCNurseryProgress aProgress,
                                                     JS::gcreason::Reason aReason)
{
  CycleCollectedJSContext* self = CycleCollectedJSContext::Get();
  MOZ_ASSERT(self->Context() == aContext);
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<TimelineConsumers> timelines = TimelineConsumers::Get();
  if (timelines && !timelines->IsEmpty()) {
    UniquePtr<AbstractTimelineMarker> abstractMarker(
      MakeUnique<MinorGCMarker>(aProgress, aReason));
    timelines->AddMarkerForAllObservedDocShells(abstractMarker);
  }

  if (self->mPrevGCNurseryCollectionCallback) {
    self->mPrevGCNurseryCollectionCallback(aContext, aProgress, aReason);
  }
}


/* static */ void
CycleCollectedJSContext::OutOfMemoryCallback(JSContext* aContext,
                                             void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);

  MOZ_ASSERT(aContext == self->Context());

  self->OnOutOfMemory();
}

/* static */ void
CycleCollectedJSContext::LargeAllocationFailureCallback(void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);

  self->OnLargeAllocationFailure();
}

/* static */ size_t
CycleCollectedJSContext::SizeofExternalStringCallback(JSString* aStr,
                                                      MallocSizeOf aMallocSizeOf)
{
  // We promised the JS engine we would not GC.  Enforce that:
  JS::AutoCheckCannotGC autoCannotGC;
  
  if (!XPCStringConvert::IsDOMString(aStr)) {
    // Might be a literal or something we don't understand.  Just claim 0.
    return 0;
  }

  const char16_t* chars = JS_GetTwoByteExternalStringChars(aStr);
  const nsStringBuffer* buf = nsStringBuffer::FromData((void*)chars);
  // We want sizeof including this, because the entire string buffer is owned by
  // the external string.  But only report here if we're unshared; if we're
  // shared then we don't know who really owns this data.
  return buf->SizeOfIncludingThisIfUnshared(aMallocSizeOf);
}

class PromiseJobRunnable final : public MicroTaskRunnable
{
public:
  PromiseJobRunnable(JS::HandleObject aCallback, JS::HandleObject aAllocationSite,
                     nsIGlobalObject* aIncumbentGlobal)
    : mCallback(new PromiseJobCallback(aCallback, aAllocationSite, aIncumbentGlobal))
  {
  }

  virtual ~PromiseJobRunnable()
  {
  }

protected:
  virtual void Run(AutoSlowOperation& aAso) override
  {
    JSObject* callback = mCallback->CallbackPreserveColor();
    nsIGlobalObject* global = callback ? xpc::NativeGlobal(callback) : nullptr;
    if (global && !global->IsDying()) {
      mCallback->Call("promise callback");
      aAso.CheckForInterrupt();
    }
  }

  virtual bool Suppressed() override
  {
    nsIGlobalObject* global =
      xpc::NativeGlobal(mCallback->CallbackPreserveColor());
    return global && global->IsInSyncOperation();
  }

private:
  RefPtr<PromiseJobCallback> mCallback;
};

/* static */
JSObject*
CycleCollectedJSContext::GetIncumbentGlobalCallback(JSContext* aCx)
{
  nsIGlobalObject* global = mozilla::dom::GetIncumbentGlobal();
  if (global) {
    return global->GetGlobalJSObject();
  }
  return nullptr;
}

/* static */
bool
CycleCollectedJSContext::EnqueuePromiseJobCallback(JSContext* aCx,
                                                   JS::HandleObject aJob,
                                                   JS::HandleObject aAllocationSite,
                                                   JS::HandleObject aIncumbentGlobal,
                                                   void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);
  MOZ_ASSERT(aCx == self->Context());
  MOZ_ASSERT(Get() == self);

  nsIGlobalObject* global = nullptr;
  if (aIncumbentGlobal) {
    global = xpc::NativeGlobal(aIncumbentGlobal);
  }
  RefPtr<MicroTaskRunnable> runnable = new PromiseJobRunnable(aJob, aAllocationSite, global);
  self->DispatchToMicroTask(runnable.forget());
  return true;
}

/* static */
void
CycleCollectedJSContext::PromiseRejectionTrackerCallback(JSContext* aCx,
                                                         JS::HandleObject aPromise,
                                                         PromiseRejectionHandlingState state,
                                                         void* aData)
{
  CycleCollectedJSContext* self = static_cast<CycleCollectedJSContext*>(aData);

  MOZ_ASSERT(aCx == self->Context());
  MOZ_ASSERT(Get() == self);

  // TODO: Bug 1549351 - Promise rejection event should not be sent for
  // cross-origin scripts


  // See HTML 8.1.6.3 HostPromiseRejectionTracker

  PromiseArray& aboutToBeNotified = self->mAboutToBeNotifiedRejectedPromises;
  PromiseHashtable& outstanding = self->mOutstandingRejections;
  uint64_t promiseID = JS::GetPromiseID(aPromise);

  if (state == PromiseRejectionHandlingState::Unhandled) {
    // Step 5.
    // Unhandled Promises first go to the about-to-be-notified queue, processed
    // after Microtasks as NotifyUnhandledRejections
    RefPtr<Promise> promise = Promise::CreateFromExisting(xpc::NativeGlobal(aPromise), aPromise);
    aboutToBeNotified.AppendElement(promise);
  } else {
    // Unhandled-then-handled Promises (ie.e Promises where the unhandledrejection event
    // handler attached a handler) may fire the rejectionhandled event.

    // Step 6.1.
    for (size_t i = 0; i < aboutToBeNotified.Length(); i++) {
      if (aboutToBeNotified[i] &&
          aboutToBeNotified[i]->PromiseObj() == aPromise) {
        // To avoid large amounts of memmoves, we don't shrink the vector
        // here. Instead, we filter out nullptrs when iterating over the
        // vector later.
        aboutToBeNotified[i] = nullptr;
        MOZ_ASSERT(!outstanding.Get(promiseID, nullptr));
        return;
      }
    }
    // Step 6.2. and 6.3.
    RefPtr<Promise> promise;
    if (outstanding.Remove(promiseID, getter_AddRefs(promise))) {
      // Step 6.4.
      nsIGlobalObject* global = xpc::NativeGlobal(aPromise);
      if (nsCOMPtr<EventTarget> owner = do_QueryInterface(global)) {
        // Step 6.5.
        // The spec says to do this in a global task, but we're just queuing an event,
        // which will execute in the next event loop either way.
        PromiseRejectionEventInit init;
        init.mPromise = Promise::CreateFromExisting(global, aPromise);
        init.mReason = JS::GetPromiseResult(aPromise);

        RefPtr<PromiseRejectionEvent> event = PromiseRejectionEvent::Constructor(owner,
                                                  NS_LITERAL_STRING("rejectionhandled"), init);

        RefPtr<AsyncEventDispatcher> asyncDispatcher = new AsyncEventDispatcher(owner, event);
        asyncDispatcher->PostDOMEvent();
      }
    }

    // Finally, notifiy debug observers
    PromiseDebugging::AddConsumedRejection(aPromise);
  }
}

struct JsGcTracer : public TraceCallbacks
{
  virtual void Trace(JS::Heap<JS::Value>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JS::Heap<jsid>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JS::Heap<JSObject*>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JSObject** aPtr, const char* aName,
                     void* aClosure) const override
  {
    js::UnsafeTraceManuallyBarrieredEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JS::TenuredHeap<JSObject*>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JS::Heap<JSString*>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JS::Heap<JSScript*>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
  virtual void Trace(JS::Heap<JSFunction*>* aPtr, const char* aName,
                     void* aClosure) const override
  {
    JS::TraceEdge(static_cast<JSTracer*>(aClosure), aPtr, aName);
  }
};

void
mozilla::TraceScriptHolder(nsISupports* aHolder, JSTracer* aTracer)
{
  nsXPCOMCycleCollectionParticipant* participant = nullptr;
  CallQueryInterface(aHolder, &participant);
  participant->Trace(aHolder, JsGcTracer(), aTracer);
}

void
CycleCollectedJSContext::TraceNativeGrayRoots(JSTracer* aTracer)
{
  MOZ_ASSERT(mJSContext);

  // NB: This is here just to preserve the existing XPConnect order. I doubt it
  // would hurt to do this after the JS holders.
  TraceAdditionalNativeGrayRoots(aTracer);

  for (auto iter = mJSHolders.Iter(); !iter.Done(); iter.Next()) {
    void* holder = iter.Key();
    nsScriptObjectTracer*& tracer = iter.Data();
    tracer->Trace(holder, JsGcTracer(), aTracer);
  }
}

void
CycleCollectedJSContext::AddJSHolder(void* aHolder, nsScriptObjectTracer* aTracer)
{
  MOZ_ASSERT(mJSContext);
  mJSHolders.Put(aHolder, aTracer);
}

struct ClearJSHolder : public TraceCallbacks
{
  virtual void Trace(JS::Heap<JS::Value>* aPtr, const char*, void*) const override
  {
    aPtr->setUndefined();
  }

  virtual void Trace(JS::Heap<jsid>* aPtr, const char*, void*) const override
  {
    *aPtr = JSID_VOID;
  }

  virtual void Trace(JS::Heap<JSObject*>* aPtr, const char*, void*) const override
  {
    *aPtr = nullptr;
  }

  virtual void Trace(JSObject** aPtr, const char* aName,
                     void* aClosure) const override
  {
    *aPtr = nullptr;
  }

  virtual void Trace(JS::TenuredHeap<JSObject*>* aPtr, const char*, void*) const override
  {
    *aPtr = nullptr;
  }

  virtual void Trace(JS::Heap<JSString*>* aPtr, const char*, void*) const override
  {
    *aPtr = nullptr;
  }

  virtual void Trace(JS::Heap<JSScript*>* aPtr, const char*, void*) const override
  {
    *aPtr = nullptr;
  }

  virtual void Trace(JS::Heap<JSFunction*>* aPtr, const char*, void*) const override
  {
    *aPtr = nullptr;
  }
};

void
CycleCollectedJSContext::RemoveJSHolder(void* aHolder)
{
  MOZ_ASSERT(mJSContext);

  nsScriptObjectTracer* tracer = mJSHolders.Get(aHolder);
  if (!tracer) {
    return;
  }
  tracer->Trace(aHolder, ClearJSHolder(), nullptr);
  mJSHolders.Remove(aHolder);
}

#ifdef DEBUG
bool
CycleCollectedJSContext::IsJSHolder(void* aHolder)
{
  MOZ_ASSERT(mJSContext);
  return mJSHolders.Get(aHolder, nullptr);
}

static void
AssertNoGcThing(JS::GCCellPtr aGCThing, const char* aName, void* aClosure)
{
  MOZ_ASSERT(!aGCThing);
}

void
CycleCollectedJSContext::AssertNoObjectsToTrace(void* aPossibleJSHolder)
{
  MOZ_ASSERT(mJSContext);

  nsScriptObjectTracer* tracer = mJSHolders.Get(aPossibleJSHolder);
  if (tracer) {
    tracer->Trace(aPossibleJSHolder, TraceCallbackFunc(AssertNoGcThing), nullptr);
  }
}
#endif

already_AddRefed<nsIException>
CycleCollectedJSContext::GetPendingException() const
{
  MOZ_ASSERT(mJSContext);

  nsCOMPtr<nsIException> out = mPendingException;
  return out.forget();
}

void
CycleCollectedJSContext::SetPendingException(nsIException* aException)
{
  MOZ_ASSERT(mJSContext);
  mPendingException = aException;
}

std::queue<RefPtr<MicroTaskRunnable>>&
CycleCollectedJSContext::GetMicroTaskQueue()
{
  MOZ_ASSERT(mJSContext);
  return mPendingMicroTaskRunnables;
}

std::queue<RefPtr<MicroTaskRunnable>>&
CycleCollectedJSContext::GetDebuggerMicroTaskQueue()
{
  MOZ_ASSERT(mJSContext);
  return mDebuggerMicroTaskQueue;
}

nsCycleCollectionParticipant*
CycleCollectedJSContext::GCThingParticipant()
{
  MOZ_ASSERT(mJSContext);
  return &mGCThingCycleCollectorGlobal;
}

nsCycleCollectionParticipant*
CycleCollectedJSContext::ZoneParticipant()
{
  MOZ_ASSERT(mJSContext);
  return &mJSZoneCycleCollectorGlobal;
}

nsresult
CycleCollectedJSContext::TraverseRoots(nsCycleCollectionNoteRootCallback& aCb)
{
  MOZ_ASSERT(mJSContext);

  TraverseNativeRoots(aCb);

  NoteWeakMapsTracer trc(mJSContext, aCb);
  js::TraceWeakMaps(&trc);

  return NS_OK;
}

bool
CycleCollectedJSContext::UsefulToMergeZones() const
{
  return false;
}

void
CycleCollectedJSContext::FixWeakMappingGrayBits() const
{
  MOZ_ASSERT(mJSContext);
  MOZ_ASSERT(!JS::IsIncrementalGCInProgress(mJSContext),
             "Don't call FixWeakMappingGrayBits during a GC.");
  FixWeakMappingGrayBitsTracer fixer(mJSContext);
  fixer.FixAll();
}

bool
CycleCollectedJSContext::AreGCGrayBitsValid() const
{
  MOZ_ASSERT(mJSContext);
  return js::AreGCGrayBitsValid(mJSContext);
}

void
CycleCollectedJSContext::GarbageCollect(uint32_t aReason) const
{
  MOZ_ASSERT(mJSContext);

  MOZ_ASSERT(aReason < JS::gcreason::NUM_REASONS);
  JS::gcreason::Reason gcreason = static_cast<JS::gcreason::Reason>(aReason);

  JS::PrepareForFullGC(mJSContext);
  JS::GCForReason(mJSContext, GC_NORMAL, gcreason);
}

void
CycleCollectedJSContext::JSObjectsTenured()
{
  MOZ_ASSERT(mJSContext);

  for (auto iter = mNurseryObjects.Iter(); !iter.Done(); iter.Next()) {
    nsWrapperCache* cache = iter.Get();
    JSObject* wrapper = cache->GetWrapperPreserveColor();
    MOZ_ASSERT(wrapper);
    if (!JS::ObjectIsTenured(wrapper)) {
      MOZ_ASSERT(!cache->PreservingWrapper());
      const JSClass* jsClass = js::GetObjectJSClass(wrapper);
      jsClass->doFinalize(nullptr, wrapper);
    }
  }

#ifdef DEBUG
for (auto iter = mPreservedNurseryObjects.Iter(); !iter.Done(); iter.Next()) {
  MOZ_ASSERT(JS::ObjectIsTenured(iter.Get().get()));
}
#endif

  mNurseryObjects.Clear();
  mPreservedNurseryObjects.Clear();
}

void
CycleCollectedJSContext::NurseryWrapperAdded(nsWrapperCache* aCache)
{
  MOZ_ASSERT(mJSContext);
  MOZ_ASSERT(aCache);
  MOZ_ASSERT(aCache->GetWrapperPreserveColor());
  MOZ_ASSERT(!JS::ObjectIsTenured(aCache->GetWrapperPreserveColor()));
  mNurseryObjects.InfallibleAppend(aCache);
}

void
CycleCollectedJSContext::NurseryWrapperPreserved(JSObject* aWrapper)
{
  MOZ_ASSERT(mJSContext);

  mPreservedNurseryObjects.InfallibleAppend(
    JS::PersistentRooted<JSObject*>(mJSContext, aWrapper));
}

void
CycleCollectedJSContext::DeferredFinalize(DeferredFinalizeAppendFunction aAppendFunc,
                                          DeferredFinalizeFunction aFunc,
                                          void* aThing)
{
  MOZ_ASSERT(mJSContext);

  void* thingArray = nullptr;
  bool hadThingArray = mDeferredFinalizerTable.Get(aFunc, &thingArray);

  thingArray = aAppendFunc(thingArray, aThing);
  if (!hadThingArray) {
    mDeferredFinalizerTable.Put(aFunc, thingArray);
  }
}

void
CycleCollectedJSContext::DeferredFinalize(nsISupports* aSupports)
{
  MOZ_ASSERT(mJSContext);

  typedef DeferredFinalizerImpl<nsISupports> Impl;
  DeferredFinalize(Impl::AppendDeferredFinalizePointer, Impl::DeferredFinalize,
                   aSupports);
}

void
CycleCollectedJSContext::DumpJSHeap(FILE* aFile)
{
  js::DumpHeap(Context(), aFile, js::CollectNurseryBeforeDump);
}

void
CycleCollectedJSContext::ProcessStableStateQueue()
{
  MOZ_ASSERT(mJSContext);
  MOZ_RELEASE_ASSERT(!mDoingStableStates);
  mDoingStableStates = true;

  // When run, one event can add another event to the mStableStateEvents, as
  // such you can't use iterators here.
  for (uint32_t i = 0; i < mStableStateEvents.Length(); ++i) {
    nsCOMPtr<nsIRunnable> event = mStableStateEvents[i].forget();
    event->Run();
  }

  mStableStateEvents.Clear();
  mDoingStableStates = false;
}

void
CycleCollectedJSContext::CleanupIDBTransactions(uint32_t aRecursionDepth)
{
  MOZ_ASSERT(mJSContext);
  MOZ_RELEASE_ASSERT(!mDoingStableStates);
  mDoingStableStates = true;

  nsTArray<PendingIDBTransactionData> localQueue = Move(mPendingIDBTransactions);

  for (uint32_t i = 0; i < localQueue.Length(); ++i)
  {
    PendingIDBTransactionData& data = localQueue[i];
    if (data.mRecursionDepth != aRecursionDepth) {
      continue;
    }

    {
      nsCOMPtr<nsIRunnable> transaction = data.mTransaction.forget();
      transaction->Run();
    }

    localQueue.RemoveElementAt(i--);
  }

  // If the queue has events in it now, they were added from something we called,
  // so they belong at the end of the queue.
  localQueue.AppendElements(mPendingIDBTransactions);
  localQueue.SwapElements(mPendingIDBTransactions);
  mDoingStableStates = false;
}

void
CycleCollectedJSContext::BeforeProcessTask(bool aMightBlock)
{
  // If ProcessNextEvent was called during a microtask callback, we
  // must process any pending microtasks before blocking in the event loop,
  // otherwise we may deadlock until an event enters the queue later.
  if (aMightBlock && PerformMicroTaskCheckPoint()) {
    // If any microtask was processed, we post a dummy event in order to
    // force the ProcessNextEvent call not to block.  This is required
    // to support nested event loops implemented using a pattern like
    // "while (condition) thread.processNextEvent(true)", in case the
    // condition is triggered here by a Promise "then" callback.
    NS_DispatchToMainThread(new Runnable());
  }
}

void
CycleCollectedJSContext::AfterProcessTask(uint32_t aRecursionDepth)
{
  MOZ_ASSERT(mJSContext);

  // See HTML 6.1.4.2 Processing model

  // Step 4.1: Execute microtasks.
  PerformMicroTaskCheckPoint();

  // Step 4.2 Execute any events that were waiting for a stable state.
  ProcessStableStateQueue();
}

void
CycleCollectedJSContext::AfterProcessMicrotasks()
{
  MOZ_ASSERT(mJSContext);
  // Notify unhandled promise rejections:
  // https://html.spec.whatwg.org/multipage/webappapis.html#notify-about-rejected-promises
  if (mAboutToBeNotifiedRejectedPromises.Length()) {
    RefPtr<NotifyUnhandledRejections> runnable = new NotifyUnhandledRejections(
        this, std::move(mAboutToBeNotifiedRejectedPromises));
    NS_DispatchToCurrentThread(runnable);
  }
  // Cleanup Indexed Database transactions:
  // https://html.spec.whatwg.org/multipage/webappapis.html#perform-a-microtask-checkpoint

  // We should only ever get here from PerformMicroTaskCheckPoint after a task or other 
  // checkpoint-able state, never from within ProcessStableStateQueue (mDoingStableStates==false).
  // However, some buggy XUL addons may dispatch JS Events from runnables in the StableState queue,
  // which then perform a checkpoint at their end, ending up here while ProcessStableStateQueue is
  // on the stack.
  // Specifically catch that here and add the call to CleanupIDBTransactions to the outer queue, to
  // be performed when we know we're in a "regular" stable state again.
  if (!mDoingStableStates) {
    CleanupIDBTransactions(RecursionDepth());
  } else {
    // Don't need a RefPtr to this here as we know this->ProcessStableStateQueue is on the stack
    nsCOMPtr<nsIRunnable> cleanupRunnable = NS_NewRunnableFunction(
      [this, rec = RecursionDepth()] {
        MOZ_ASSERT(mDoingStableStates);
        // As this is called from ProcessStableStateQueue, mDoingStableStates == true.
        // Switch the flag while CleanupIDBTransactions executes.
        mDoingStableStates = false;
        CleanupIDBTransactions(rec);
        mDoingStableStates = true;
      });
    RunInStableState(cleanupRunnable.forget());
  };
}

uint32_t
CycleCollectedJSContext::RecursionDepth() const
{
  return mOwningThread->RecursionDepth();
}

void
CycleCollectedJSContext::RunInStableState(already_AddRefed<nsIRunnable>&& aRunnable)
{
  MOZ_ASSERT(mJSContext);
  mStableStateEvents.AppendElement(Move(aRunnable));
}

void
CycleCollectedJSContext::AddPendingIDBTransaction(already_AddRefed<nsIRunnable>&& aTransaction)
{
  MOZ_ASSERT(mJSContext);

  PendingIDBTransactionData data;
  data.mTransaction = aTransaction;

  MOZ_ASSERT(mOwningThread);
  data.mRecursionDepth = RecursionDepth();

  // There must be an event running to get here.
#ifndef MOZ_WIDGET_COCOA
  MOZ_ASSERT(data.mRecursionDepth > mBaseRecursionDepth);
#else
  // XXX bug 1261143
  // Recursion depth should be greater than mBaseRecursionDepth,
  // or the runnable will stay in the queue forever.
  if (data.mRecursionDepth <= mBaseRecursionDepth) {
    data.mRecursionDepth = mBaseRecursionDepth + 1;
  }
#endif

  mPendingIDBTransactions.AppendElement(Move(data));
}

IncrementalFinalizeRunnable::IncrementalFinalizeRunnable(CycleCollectedJSContext* aCx,
                                                         DeferredFinalizerTable& aFinalizers)
  : mContext(aCx)
  , mFinalizeFunctionToRun(0)
  , mReleasing(false)
{
  for (auto iter = aFinalizers.Iter(); !iter.Done(); iter.Next()) {
    DeferredFinalizeFunction& function = iter.Key();
    void*& data = iter.Data();

    DeferredFinalizeFunctionHolder* holder =
      mDeferredFinalizeFunctions.AppendElement();
    holder->run = function;
    holder->data = data;

    iter.Remove();
  }
}

IncrementalFinalizeRunnable::~IncrementalFinalizeRunnable()
{
  MOZ_ASSERT(this != mContext->mFinalizeRunnable);
}

void
IncrementalFinalizeRunnable::ReleaseNow(bool aLimited)
{
  if (mReleasing) {
    NS_WARNING("Re-entering ReleaseNow");
    return;
  }
  {
    mozilla::AutoRestore<bool> ar(mReleasing);
    mReleasing = true;
    MOZ_ASSERT(mDeferredFinalizeFunctions.Length() != 0,
               "We should have at least ReleaseSliceNow to run");
    MOZ_ASSERT(mFinalizeFunctionToRun < mDeferredFinalizeFunctions.Length(),
               "No more finalizers to run?");

    TimeDuration sliceTime = TimeDuration::FromMilliseconds(SliceMillis);
    TimeStamp started = TimeStamp::Now();
    bool timeout = false;
    do {
      const DeferredFinalizeFunctionHolder& function =
        mDeferredFinalizeFunctions[mFinalizeFunctionToRun];
      if (aLimited) {
        bool done = false;
        while (!timeout && !done) {
          /*
           * We don't want to read the clock too often, so we try to
           * release slices of 100 items.
           */
          done = function.run(100, function.data);
          timeout = TimeStamp::Now() - started >= sliceTime;
        }
        if (done) {
          ++mFinalizeFunctionToRun;
        }
        if (timeout) {
          break;
        }
      } else {
        while (!function.run(UINT32_MAX, function.data));
        ++mFinalizeFunctionToRun;
      }
    } while (mFinalizeFunctionToRun < mDeferredFinalizeFunctions.Length());
  }

  if (mFinalizeFunctionToRun == mDeferredFinalizeFunctions.Length()) {
    MOZ_ASSERT(mContext->mFinalizeRunnable == this);
    mDeferredFinalizeFunctions.Clear();
    // NB: This may delete this!
    mContext->mFinalizeRunnable = nullptr;
  }
}

NS_IMETHODIMP
IncrementalFinalizeRunnable::Run()
{
  if (mContext->mFinalizeRunnable != this) {
    /* These items were already processed synchronously in JSGC_END. */
    MOZ_ASSERT(!mDeferredFinalizeFunctions.Length());
    return NS_OK;
  }

  TimeStamp start = TimeStamp::Now();
  ReleaseNow(true);

  if (mDeferredFinalizeFunctions.Length()) {
    nsresult rv = NS_DispatchToCurrentThread(this);
    if (NS_FAILED(rv)) {
      ReleaseNow(false);
    }
  }

  uint32_t duration = (uint32_t)((TimeStamp::Now() - start).ToMilliseconds());
  Telemetry::Accumulate(Telemetry::DEFERRED_FINALIZE_ASYNC, duration);

  return NS_OK;
}

void
CycleCollectedJSContext::FinalizeDeferredThings(DeferredFinalizeType aType)
{
  MOZ_ASSERT(mJSContext);

  /*
   * If the previous GC created a runnable to finalize objects
   * incrementally, and if it hasn't finished yet, finish it now. We
   * don't want these to build up. We also don't want to allow any
   * existing incremental finalize runnables to run after a
   * non-incremental GC, since they are often used to detect leaks.
   */
  if (mFinalizeRunnable) {
    mFinalizeRunnable->ReleaseNow(false);
    if (mFinalizeRunnable) {
      // If we re-entered ReleaseNow, we couldn't delete mFinalizeRunnable and
      // we need to just continue processing it.
      return;
    }
  }

  if (mDeferredFinalizerTable.Count() == 0) {
    return;
  }

  mFinalizeRunnable = new IncrementalFinalizeRunnable(this,
                                                      mDeferredFinalizerTable);

  // Everything should be gone now.
  MOZ_ASSERT(mDeferredFinalizerTable.Count() == 0);

  if (aType == FinalizeIncrementally) {
    NS_DispatchToCurrentThread(mFinalizeRunnable);
  } else {
    mFinalizeRunnable->ReleaseNow(false);
    MOZ_ASSERT(!mFinalizeRunnable);
  }
}

void
CycleCollectedJSContext::AnnotateAndSetOutOfMemory(OOMState* aStatePtr,
                                                   OOMState aNewState)
{
  MOZ_ASSERT(mJSContext);

  *aStatePtr = aNewState;
}

void
CycleCollectedJSContext::OnGC(JSGCStatus aStatus)
{
  MOZ_ASSERT(mJSContext);

  switch (aStatus) {
    case JSGC_BEGIN:
      nsCycleCollector_prepareForGarbageCollection();
      mZonesWaitingForGC.Clear();
      break;
    case JSGC_END: {
      // Do any deferred finalization of native objects.
      FinalizeDeferredThings(JS::WasIncrementalGC(mJSContext) ? FinalizeIncrementally :
                                                                FinalizeNow);
      break;
    }
    default:
      MOZ_CRASH();
  }

  CustomGCCallback(aStatus);
}

void
CycleCollectedJSContext::OnOutOfMemory()
{
  MOZ_ASSERT(mJSContext);

  AnnotateAndSetOutOfMemory(&mOutOfMemoryState, OOMState::Reporting);
  CustomOutOfMemoryCallback();
  AnnotateAndSetOutOfMemory(&mOutOfMemoryState, OOMState::Reported);
}

void
CycleCollectedJSContext::OnLargeAllocationFailure()
{
  MOZ_ASSERT(mJSContext);

  AnnotateAndSetOutOfMemory(&mLargeAllocationFailureState, OOMState::Reporting);
  CustomLargeAllocationFailureCallback();
  AnnotateAndSetOutOfMemory(&mLargeAllocationFailureState, OOMState::Reported);
}

void
CycleCollectedJSContext::PrepareWaitingZonesForGC()
{
  if (mZonesWaitingForGC.Count() == 0) {
    JS::PrepareForFullGC(Context());
  } else {
    for (auto iter = mZonesWaitingForGC.Iter(); !iter.Done(); iter.Next()) {
      JS::PrepareZoneForGC(iter.Get()->GetKey());
    }
    mZonesWaitingForGC.Clear();
  }
}

void
CycleCollectedJSContext::DispatchToMicroTask(already_AddRefed<MicroTaskRunnable> aRunnable)
{
  RefPtr<MicroTaskRunnable> runnable(aRunnable);

  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(runnable);

  mPendingMicroTaskRunnables.push(runnable.forget());
}

class AsyncMutationHandler final : public mozilla::Runnable
{
public:
  NS_IMETHOD Run() override
  {
    CycleCollectedJSContext* ccjs = CycleCollectedJSContext::Get();
    if (ccjs) {
      ccjs->PerformMicroTaskCheckPoint();
    }
    return NS_OK;
  }
};

bool
CycleCollectedJSContext::PerformMicroTaskCheckPoint()
{
  if (mPendingMicroTaskRunnables.empty() && mDebuggerMicroTaskQueue.empty()) {
    AfterProcessMicrotasks();
    // Nothing to do, return early.
    return false;
  }

  uint32_t currentDepth = RecursionDepth();
  if (mMicroTaskRecursionDepth >= currentDepth) {
    // We are already executing microtasks for the current recursion depth.
    return false;
  }

  if (mTargetedMicroTaskRecursionDepth != 0 &&
      mTargetedMicroTaskRecursionDepth != currentDepth) {
    return false;
  }

  if (NS_IsMainThread() && !nsContentUtils::IsSafeToRunScript()) {
    // Special case for main thread where DOM mutations may happen when
    // it is not safe to run scripts.
    nsContentUtils::AddScriptRunner(new AsyncMutationHandler());
    return false;
  }

  mozilla::AutoRestore<uint32_t> restore(mMicroTaskRecursionDepth);
  MOZ_ASSERT(currentDepth > 0);
  mMicroTaskRecursionDepth = currentDepth;

  bool didProcess = false;
  AutoSlowOperation aso;

  std::queue<RefPtr<MicroTaskRunnable>> suppressed;
  for (;;) {
    RefPtr<MicroTaskRunnable> runnable;
    if (!mDebuggerMicroTaskQueue.empty()) {
      runnable = mDebuggerMicroTaskQueue.front().forget();
      mDebuggerMicroTaskQueue.pop();
    } else if (!mPendingMicroTaskRunnables.empty()) {
      runnable = mPendingMicroTaskRunnables.front().forget();
      mPendingMicroTaskRunnables.pop();
    } else {
      break;
    }

    if (runnable->Suppressed()) {
      // Microtasks in worker shall never be suppressed.
      // Otherwise, mPendingMicroTaskRunnables will be replaced later with
      // all suppressed tasks in mDebuggerMicroTaskQueue unexpectedly.
      MOZ_ASSERT(NS_IsMainThread());
      suppressed.push(runnable);
    } else {
      didProcess = true;
      runnable->Run(aso);
    }
  }

  // Put back the suppressed microtasks so that they will be run later.
  // Note, it is possible that we end up keeping these suppressed tasks around
  // for some time, but no longer than spinning the event loop nestedly
  // (sync XHR, alert, etc.)
  mPendingMicroTaskRunnables.swap(suppressed);

  AfterProcessMicrotasks();

  return didProcess;
}

void
CycleCollectedJSContext::PerformDebuggerMicroTaskCheckpoint()
 {
  // Don't do normal microtask handling checks here, since whoever is calling
  // this method is supposed to know what they are doing.

  AutoSlowOperation aso;
  for (;;) {
    // For a debugger microtask checkpoint, we always use the debugger microtask
    // queue.
    std::queue<RefPtr<MicroTaskRunnable>>* microtaskQueue =
      &GetDebuggerMicroTaskQueue();

    if (microtaskQueue->empty()) {
      break;
    }

    RefPtr<MicroTaskRunnable> runnable = microtaskQueue->front().forget();
    MOZ_ASSERT(runnable);

    // This function can re-enter, so we remove the element before calling.
    microtaskQueue->pop();
    runnable->Run(aso);
  }

  AfterProcessMicrotasks();
}

NS_IMETHODIMP
CycleCollectedJSContext::NotifyUnhandledRejections::Run()
{
  // See HTML 8.1.4.7 Unhandled promise rejections
  // with 'mUnhandledRejections' == copy of 'about-to-be-notified rejected'

  for (size_t i = 0; i < mUnhandledRejections.Length(); ++i) {
    // Step 5.1.
    RefPtr<Promise>& promise = mUnhandledRejections[i];
    if (!promise)
      continue;

    JS::RootedObject promiseObj(mCx->RootingCx(), promise->PromiseObj());
    MOZ_ASSERT(JS::IsPromiseObject(promiseObj));

    // Step 5.1.1.
    if (JS::GetPromiseIsHandled(promiseObj))
      continue;

    uint64_t promiseID = JS::GetPromiseID(promiseObj);

    // Step 5.1.2.
    bool raiseError = true;
    if (nsCOMPtr<EventTarget> target =
            do_QueryInterface(promise->GetParentObject())) {
      PromiseRejectionEventInit init;
      init.mPromise = promise;
      init.mReason = JS::GetPromiseResult(promiseObj);
      init.mCancelable = true;

      RefPtr<PromiseRejectionEvent> event =
          PromiseRejectionEvent::Constructor(
              target, NS_LITERAL_STRING("unhandledrejection"), init);

      // Step 5.1.4. (reordered)
      // In contrast to the letter of the spec, this must be done before
      // the event handler is called or the PromiseRejectionTrackerCallback
      // will not see the correct state.
      PromiseDebugging::AddUncaughtRejection(promiseObj);
      target->DispatchEvent(event, &raiseError);
    }

    // Step 5.1.3. (implied)
    // If the Promise became handled, PromiseRejectionTrackerCallback will have marked it.
    // If not, keep it in the "uncaught" list for PromiseDebugging, and set the flag if
    // preventDefault indicated we don't want it reported to console.
    if (!JS::GetPromiseIsHandled(promiseObj) && !raiseError) {
      JS::MarkPromiseRejectionReported(promiseObj);
    }
  }
  return NS_OK;
}

nsresult
CycleCollectedJSContext::NotifyUnhandledRejections::Cancel()
{
  for (size_t i = 0; i < mUnhandledRejections.Length(); ++i) {
    RefPtr<Promise>& promise = mUnhandledRejections[i];
    if (!promise)
      continue;

    JS::RootedObject promiseObj(mCx->RootingCx(), promise->PromiseObj());
    mCx->mOutstandingRejections.Remove(JS::GetPromiseID(promiseObj));
  }
  return NS_OK;
}

void
CycleCollectedJSContext::EnvironmentPreparer::invoke(JS::HandleObject scope,
                                                     js::ScriptEnvironmentPreparer::Closure& closure)
{
  nsIGlobalObject* global = xpc::NativeGlobal(scope);

  // Not much we can do if we simply don't have a usable global here...
  NS_ENSURE_TRUE_VOID(global && global->GetGlobalJSObject());

  AutoEntryScript aes(global, "JS-engine-initiated execution");

  MOZ_ASSERT(!JS_IsExceptionPending(aes.cx()));

  DebugOnly<bool> ok = closure(aes.cx());

  MOZ_ASSERT_IF(ok, !JS_IsExceptionPending(aes.cx()));

  // The AutoEntryScript will check for JS_IsExceptionPending on the
  // JSContext and report it as needed as it comes off the stack.
}
