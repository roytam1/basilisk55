/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_CycleCollectedJSContext_h__
#define mozilla_CycleCollectedJSContext_h__

#include <queue>

#include "mozilla/DeferredFinalize.h"
#include "mozilla/mozalloc.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/SegmentedVector.h"
#include "mozilla/dom/Promise.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#include "nsCycleCollectionParticipant.h"
#include "nsDataHashtable.h"
#include "nsHashKeys.h"
#include "nsTArray.h"
#include "nsTHashtable.h"

class nsCycleCollectionNoteRootCallback;
class nsIException;
class nsIRunnable;
class nsThread;
class nsWrapperCache;

namespace js {
struct Class;
} // namespace js

namespace mozilla {
class AutoSlowOperation;

class JSGCThingParticipant: public nsCycleCollectionParticipant
{
public:
  NS_IMETHOD_(void) Root(void*) override
  {
    MOZ_ASSERT(false, "Don't call Root on GC things");
  }

  NS_IMETHOD_(void) Unlink(void*) override
  {
    MOZ_ASSERT(false, "Don't call Unlink on GC things, as they may be dead");
  }

  NS_IMETHOD_(void) Unroot(void*) override
  {
    MOZ_ASSERT(false, "Don't call Unroot on GC things, as they may be dead");
  }

  NS_IMETHOD_(void) DeleteCycleCollectable(void* aPtr) override
  {
    MOZ_ASSERT(false, "Can't directly delete a cycle collectable GC thing");
  }

  NS_IMETHOD TraverseNative(void* aPtr, nsCycleCollectionTraversalCallback& aCb)
    override;

  NS_DECL_CYCLE_COLLECTION_CLASS_NAME_METHOD(JSGCThingParticipant)
};

class JSZoneParticipant : public nsCycleCollectionParticipant
{
public:
  constexpr JSZoneParticipant(): nsCycleCollectionParticipant()
  {
  }

  NS_IMETHOD_(void) Root(void*) override
  {
    MOZ_ASSERT(false, "Don't call Root on GC things");
  }

  NS_IMETHOD_(void) Unlink(void*) override
  {
    MOZ_ASSERT(false, "Don't call Unlink on GC things, as they may be dead");
  }

  NS_IMETHOD_(void) Unroot(void*) override
  {
    MOZ_ASSERT(false, "Don't call Unroot on GC things, as they may be dead");
  }

  NS_IMETHOD_(void) DeleteCycleCollectable(void*) override
  {
    MOZ_ASSERT(false, "Can't directly delete a cycle collectable GC thing");
  }

  NS_IMETHOD TraverseNative(void* aPtr, nsCycleCollectionTraversalCallback& aCb)
    override;

  NS_DECL_CYCLE_COLLECTION_CLASS_NAME_METHOD(JSZoneParticipant)
};

class IncrementalFinalizeRunnable;

// Contains various stats about the cycle collection.
struct CycleCollectorResults
{
  CycleCollectorResults()
  {
    // Initialize here so when we increment mNumSlices the first time we're
    // not using uninitialized memory.
    Init();
  }

  void Init()
  {
    mForcedGC = false;
    mMergedZones = false;
    mAnyManual = false;
    mVisitedRefCounted = 0;
    mVisitedGCed = 0;
    mFreedRefCounted = 0;
    mFreedGCed = 0;
    mFreedJSZones = 0;
    mNumSlices = 1;
    // mNumSlices is initialized to one, because we call Init() after the
    // per-slice increment of mNumSlices has already occurred.
  }

  bool mForcedGC;
  bool mMergedZones;
  bool mAnyManual; // true if any slice of the CC was manually triggered, or at shutdown.
  uint32_t mVisitedRefCounted;
  uint32_t mVisitedGCed;
  uint32_t mFreedRefCounted;
  uint32_t mFreedGCed;
  uint32_t mFreedJSZones;
  uint32_t mNumSlices;
};

class MicroTaskRunnable
{
public:
  MicroTaskRunnable() = default;
  NS_INLINE_DECL_REFCOUNTING(MicroTaskRunnable)
  virtual void Run(AutoSlowOperation& aAso) = 0;
  virtual bool Suppressed() { return false; }
protected:
  virtual ~MicroTaskRunnable() {}
};

class CycleCollectedJSContext
{
  friend class JSGCThingParticipant;
  friend class JSZoneParticipant;
  friend class IncrementalFinalizeRunnable;
protected:
  CycleCollectedJSContext();
  virtual ~CycleCollectedJSContext();

  MOZ_IS_CLASS_INIT
  nsresult Initialize(JSContext* aParentContext,
                      uint32_t aMaxBytes,
                      uint32_t aMaxNurseryBytes);

  size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;
  void UnmarkSkippableJSHolders();

  virtual void
  TraverseAdditionalNativeRoots(nsCycleCollectionNoteRootCallback& aCb) {}
  virtual void TraceAdditionalNativeGrayRoots(JSTracer* aTracer) {}

  virtual void CustomGCCallback(JSGCStatus aStatus) {}
  virtual void CustomOutOfMemoryCallback() {}
  virtual void CustomLargeAllocationFailureCallback() {}

private:
  void
  DescribeGCThing(bool aIsMarked, JS::GCCellPtr aThing,
                  nsCycleCollectionTraversalCallback& aCb) const;

  virtual bool
  DescribeCustomObjects(JSObject* aObject, const js::Class* aClasp,
                        char (&aName)[72]) const
  {
    return false; // We did nothing.
  }

  void
  NoteGCThingJSChildren(JS::GCCellPtr aThing,
                        nsCycleCollectionTraversalCallback& aCb) const;

  void
  NoteGCThingXPCOMChildren(const js::Class* aClasp, JSObject* aObj,
                           nsCycleCollectionTraversalCallback& aCb) const;

  virtual bool
  NoteCustomGCThingXPCOMChildren(const js::Class* aClasp, JSObject* aObj,
                                 nsCycleCollectionTraversalCallback& aCb) const
  {
    return false; // We did nothing.
  }

  enum TraverseSelect {
    TRAVERSE_CPP,
    TRAVERSE_FULL
  };

  void
  TraverseGCThing(TraverseSelect aTs, JS::GCCellPtr aThing,
                  nsCycleCollectionTraversalCallback& aCb);

  void
  TraverseZone(JS::Zone* aZone, nsCycleCollectionTraversalCallback& aCb);

  static void
  TraverseObjectShim(void* aData, JS::GCCellPtr aThing);

  void TraverseNativeRoots(nsCycleCollectionNoteRootCallback& aCb);

  static void TraceBlackJS(JSTracer* aTracer, void* aData);
  static void TraceGrayJS(JSTracer* aTracer, void* aData);
  static void GCCallback(JSContext* aContext, JSGCStatus aStatus, void* aData);
  static void GCSliceCallback(JSContext* aContext, JS::GCProgress aProgress,
                              const JS::GCDescription& aDesc);
  static void GCNurseryCollectionCallback(JSContext* aContext,
                                          JS::GCNurseryProgress aProgress,
                                          JS::gcreason::Reason aReason);
  static void OutOfMemoryCallback(JSContext* aContext, void* aData);
  static void LargeAllocationFailureCallback(void* aData);
  /**
   * Callback for reporting external string memory.
   */
  static size_t SizeofExternalStringCallback(JSString* aStr,
                                             mozilla::MallocSizeOf aMallocSizeOf);

  static bool ContextCallback(JSContext* aCx, unsigned aOperation,
                              void* aData);
  static JSObject* GetIncumbentGlobalCallback(JSContext* aCx);
  static bool EnqueuePromiseJobCallback(JSContext* aCx,
                                        JS::HandleObject aJob,
                                        JS::HandleObject aAllocationSite,
                                        JS::HandleObject aIncumbentGlobal,
                                        void* aData);
  static void PromiseRejectionTrackerCallback(JSContext* aCx,
                                              JS::HandleObject aPromise,
                                              PromiseRejectionHandlingState state,
                                              void* aData);

  virtual void TraceNativeBlackRoots(JSTracer* aTracer) { };
  void TraceNativeGrayRoots(JSTracer* aTracer);

  void AfterProcessMicrotasks();
public:
  void ProcessStableStateQueue();
private:
  void CleanupIDBTransactions(uint32_t aRecursionDepth);

public:
  enum DeferredFinalizeType {
    FinalizeIncrementally,
    FinalizeNow,
  };

  void FinalizeDeferredThings(DeferredFinalizeType aType);

  // Two conditions, JSOutOfMemory and JSLargeAllocationFailure, are noted in
  // crash reports. Here are the values that can appear in the reports:
  enum class OOMState : uint32_t {
    // The condition has never happened. No entry appears in the crash report.
    OK,

    // We are currently reporting the given condition.
    //
    // Suppose a crash report contains "JSLargeAllocationFailure:
    // Reporting". This means we crashed while executing memory-pressure
    // observers, trying to shake loose some memory. The large allocation in
    // question did not return null: it is still on the stack. Had we not
    // crashed, it would have been retried.
    Reporting,

    // The condition has been reported since the last GC.
    //
    // If a crash report contains "JSOutOfMemory: Reported", that means a small
    // allocation failed, and then we crashed, probably due to buggy
    // error-handling code that ran after allocation returned null.
    //
    // This contrasts with "Reporting" which means that no error-handling code
    // had executed yet.
    Reported,

    // The condition has happened, but a GC cycle ended since then.
    //
    // GC is taken as a proxy for "we've been banging on the heap a good bit
    // now and haven't crashed; the OOM was probably handled correctly".
    Recovered
  };

private:
  void AnnotateAndSetOutOfMemory(OOMState* aStatePtr, OOMState aNewState);
  void OnGC(JSGCStatus aStatus);
  void OnOutOfMemory();
  void OnLargeAllocationFailure();

public:
  void AddJSHolder(void* aHolder, nsScriptObjectTracer* aTracer);
  void RemoveJSHolder(void* aHolder);
#ifdef DEBUG
  bool IsJSHolder(void* aHolder);
  void AssertNoObjectsToTrace(void* aPossibleJSHolder);
#endif

  already_AddRefed<nsIException> GetPendingException() const;
  void SetPendingException(nsIException* aException);

  std::queue<RefPtr<MicroTaskRunnable>>& GetMicroTaskQueue();
  std::queue<RefPtr<MicroTaskRunnable>>& GetDebuggerMicroTaskQueue();

  nsCycleCollectionParticipant* GCThingParticipant();
  nsCycleCollectionParticipant* ZoneParticipant();

  nsresult TraverseRoots(nsCycleCollectionNoteRootCallback& aCb);
  virtual bool UsefulToMergeZones() const;
  void FixWeakMappingGrayBits() const;
  bool AreGCGrayBitsValid() const;
  void GarbageCollect(uint32_t aReason) const;

  void NurseryWrapperAdded(nsWrapperCache* aCache);
  void NurseryWrapperPreserved(JSObject* aWrapper);
  void JSObjectsTenured();

  void DeferredFinalize(DeferredFinalizeAppendFunction aAppendFunc,
                        DeferredFinalizeFunction aFunc,
                        void* aThing);
  void DeferredFinalize(nsISupports* aSupports);

  void DumpJSHeap(FILE* aFile);

  virtual void PrepareForForgetSkippable() = 0;
  virtual void BeginCycleCollectionCallback() = 0;
  virtual void EndCycleCollectionCallback(CycleCollectorResults& aResults) = 0;
  virtual void DispatchDeferredDeletion(bool aContinuation, bool aPurge = false) = 0;

  JSContext* Context() const
  {
    MOZ_ASSERT(mJSContext);
    return mJSContext;
  }

  JS::RootingContext* RootingCx() const
  {
    MOZ_ASSERT(mJSContext);
    return JS::RootingContext::get(mJSContext);
  }

  void SetTargetedMicroTaskRecursionDepth(uint32_t aDepth)
  {
    mTargetedMicroTaskRecursionDepth = aDepth;
  }

protected:
  JSContext* MaybeContext() const { return mJSContext; }

public:
  // nsThread entrypoints
  virtual void BeforeProcessTask(bool aMightBlock);
  virtual void AfterProcessTask(uint32_t aRecursionDepth);

  uint32_t RecursionDepth() const;

  // Run in stable state (call through nsContentUtils)
  void RunInStableState(already_AddRefed<nsIRunnable>&& aRunnable);

  void AddPendingIDBTransaction(already_AddRefed<nsIRunnable>&& aTransaction);

  // Get the current thread's CycleCollectedJSContext.  Returns null if there
  // isn't one.
  static CycleCollectedJSContext* Get();

  // Add aZone to the set of zones waiting for a GC.
  void AddZoneWaitingForGC(JS::Zone* aZone)
  {
    mZonesWaitingForGC.PutEntry(aZone);
  }

  // Prepare any zones for GC that have been passed to AddZoneWaitingForGC()
  // since the last GC or since the last call to PrepareWaitingZonesForGC(),
  // whichever was most recent. If there were no such zones, prepare for a
  // full GC.
  void PrepareWaitingZonesForGC();

  // Queue an async microtask to the current main or worker thread.
  virtual void DispatchToMicroTask(already_AddRefed<MicroTaskRunnable> aRunnable);

  // Call EnterMicroTask when you're entering JS execution.
  // Usually the best way to do this is to use nsAutoMicroTask.
  void EnterMicroTask()
  {
    ++mMicroTaskLevel;
  }

  void LeaveMicroTask()
  {
    if (--mMicroTaskLevel == 0) {
      PerformMicroTaskCheckPoint();
    }
  }

  bool IsInMicroTask() const
  {
    return mMicroTaskLevel != 0;
  }

  uint32_t MicroTaskLevel() const
  {
    return mMicroTaskLevel;
  }

  void SetMicroTaskLevel(uint32_t aLevel)
  {
    mMicroTaskLevel = aLevel;
  }

  bool PerformMicroTaskCheckPoint();

  void PerformDebuggerMicroTaskCheckpoint();

  bool IsInStableOrMetaStableState() const
  {
    return mDoingStableStates;
  }

  // Storage for watching rejected promises waiting for some client to
  // consume their rejection.
  // Promises in this list have been rejected in the last turn of the
  // event loop without the rejection being handled.
  // Note that this can contain nullptrs in place of promises removed because
  // they're consumed before it'd be reported.
  JS::PersistentRooted<JS::GCVector<JSObject*, 0, js::SystemAllocPolicy>> mUncaughtRejections;

  // Promises in this list have previously been reported as rejected
  // (because they were in the above list), but the rejection was handled
  // in the last turn of the event loop.
  JS::PersistentRooted<JS::GCVector<JSObject*, 0, js::SystemAllocPolicy>> mConsumedRejections;

  // This is for the "outstanding rejected promises weak set" in the spec,
  // https://html.spec.whatwg.org/multipage/webappapis.html#outstanding-rejected-promises-weak-set
  typedef nsRefPtrHashtable<nsUint64HashKey, dom::Promise> PromiseHashtable;
  PromiseHashtable mOutstandingRejections;


  nsTArray<nsCOMPtr<nsISupports /* UncaughtRejectionObserver */ >> mUncaughtRejectionObservers;

private:
  JSGCThingParticipant mGCThingCycleCollectorGlobal;

  JSZoneParticipant mJSZoneCycleCollectorGlobal;

  JSContext* mJSContext;

  JS::GCSliceCallback mPrevGCSliceCallback;
  JS::GCNurseryCollectionCallback mPrevGCNurseryCollectionCallback;

  nsDataHashtable<nsPtrHashKey<void>, nsScriptObjectTracer*> mJSHolders;

  typedef nsDataHashtable<nsFuncPtrHashKey<DeferredFinalizeFunction>, void*>
    DeferredFinalizerTable;
  DeferredFinalizerTable mDeferredFinalizerTable;

  RefPtr<IncrementalFinalizeRunnable> mFinalizeRunnable;

  nsCOMPtr<nsIException> mPendingException;
  nsThread* mOwningThread; // Manual refcounting to avoid include hell.

  struct PendingIDBTransactionData
  {
    nsCOMPtr<nsIRunnable> mTransaction;
    uint32_t mRecursionDepth;
  };

  nsTArray<nsCOMPtr<nsIRunnable>> mStableStateEvents;
  nsTArray<PendingIDBTransactionData> mPendingIDBTransactions;
  uint32_t mBaseRecursionDepth;
  bool mDoingStableStates;

  // If set to none 0, microtasks will be processed only when recursion depth
  // is the set value.
  uint32_t mTargetedMicroTaskRecursionDepth;

  uint32_t mMicroTaskLevel;

  std::queue<RefPtr<MicroTaskRunnable>> mPendingMicroTaskRunnables;
  std::queue<RefPtr<MicroTaskRunnable>> mDebuggerMicroTaskQueue;

  uint32_t mMicroTaskRecursionDepth;

  // This implements about-to-be-notified rejected promises list in the spec.
  // https://html.spec.whatwg.org/multipage/webappapis.html#about-to-be-notified-rejected-promises-list
  typedef nsTArray<RefPtr<dom::Promise>> PromiseArray;
  PromiseArray mAboutToBeNotifiedRejectedPromises;

  class NotifyUnhandledRejections final : public CancelableRunnable {
   public:
    NotifyUnhandledRejections(CycleCollectedJSContext* aCx,
                              PromiseArray&& aPromises)
        : CancelableRunnable(),
          mCx(aCx),
          mUnhandledRejections(std::move(aPromises)) {}

    NS_IMETHOD Run() final;

    nsresult Cancel() final;

   private:
    CycleCollectedJSContext* mCx;
    PromiseArray mUnhandledRejections;
  };

  OOMState mOutOfMemoryState;
  OOMState mLargeAllocationFailureState;

  static const size_t kSegmentSize = 512;
  SegmentedVector<nsWrapperCache*, kSegmentSize, InfallibleAllocPolicy>
    mNurseryObjects;
  SegmentedVector<JS::PersistentRooted<JSObject*>, kSegmentSize,
                  InfallibleAllocPolicy>
    mPreservedNurseryObjects;

  nsTHashtable<nsPtrHashKey<JS::Zone>> mZonesWaitingForGC;

  struct EnvironmentPreparer : public js::ScriptEnvironmentPreparer {
    void invoke(JS::HandleObject scope, Closure& closure) override;
  };
  EnvironmentPreparer mEnvironmentPreparer;
};

class MOZ_STACK_CLASS nsAutoMicroTask
{
public:
  nsAutoMicroTask()
  {
    CycleCollectedJSContext* ccjs = CycleCollectedJSContext::Get();
    if (ccjs) {
      ccjs->EnterMicroTask();
    }
  }
  ~nsAutoMicroTask()
  {
    CycleCollectedJSContext* ccjs = CycleCollectedJSContext::Get();
    if (ccjs) {
      ccjs->LeaveMicroTask();
    }
  }
};

void TraceScriptHolder(nsISupports* aHolder, JSTracer* aTracer);

// Returns true if the JS::TraceKind is one the cycle collector cares about.
inline bool AddToCCKind(JS::TraceKind aKind)
{
  return aKind == JS::TraceKind::Object ||
         aKind == JS::TraceKind::Script ||
         aKind == JS::TraceKind::Scope ||
         aKind == JS::TraceKind::RegExpShared;
}

bool
GetBuildId(JS::BuildIdCharVector* aBuildID);

} // namespace mozilla

#endif // mozilla_CycleCollectedJSContext_h__
