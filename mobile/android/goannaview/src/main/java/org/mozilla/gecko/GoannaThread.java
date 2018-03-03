/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna;

import org.mozilla.goanna.annotation.RobocopTarget;
import org.mozilla.goanna.annotation.WrapForJNI;
import org.mozilla.goanna.mozglue.GoannaLoader;
import org.mozilla.goanna.util.FileUtils;
import org.mozilla.goanna.util.ThreadUtils;

import org.json.JSONException;
import org.json.JSONObject;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.MessageQueue;
import android.os.SystemClock;
import android.text.TextUtils;
import android.util.Log;

import java.io.File;
import java.io.FilenameFilter;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Locale;
import java.util.Queue;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.StringTokenizer;

public class GoannaThread extends Thread {
    private static final String LOGTAG = "GoannaThread";

    public enum State {
        // After being loaded by class loader.
        @WrapForJNI INITIAL(0),
        // After launching Goanna thread
        @WrapForJNI LAUNCHED(1),
        // After loading the mozglue library.
        @WrapForJNI MOZGLUE_READY(2),
        // After loading the libxul library.
        @WrapForJNI LIBS_READY(3),
        // After initializing nsAppShell and JNI calls.
        @WrapForJNI JNI_READY(4),
        // After initializing profile and prefs.
        @WrapForJNI PROFILE_READY(5),
        // After initializing frontend JS
        @WrapForJNI RUNNING(6),
        // After leaving Goanna event loop
        @WrapForJNI EXITING(3),
        // After exiting GoannaThread (corresponding to "Goanna:Exited" event)
        @WrapForJNI EXITED(0);

        /* The rank is an arbitrary value reflecting the amount of components or features
         * that are available for use. During startup and up to the RUNNING state, the
         * rank value increases because more components are initialized and available for
         * use. During shutdown and up to the EXITED state, the rank value decreases as
         * components are shut down and become unavailable. EXITING has the same rank as
         * LIBS_READY because both states have a similar amount of components available.
         */
        private final int rank;

        private State(int rank) {
            this.rank = rank;
        }

        public boolean is(final State other) {
            return this == other;
        }

        public boolean isAtLeast(final State other) {
            return this.rank >= other.rank;
        }

        public boolean isAtMost(final State other) {
            return this.rank <= other.rank;
        }

        // Inclusive
        public boolean isBetween(final State min, final State max) {
            return this.rank >= min.rank && this.rank <= max.rank;
        }
    }

    public static final State MIN_STATE = State.INITIAL;
    public static final State MAX_STATE = State.EXITED;

    private static volatile State sState = State.INITIAL;

    private static class QueuedCall {
        public Method method;
        public Object target;
        public Object[] args;
        public State state;

        public QueuedCall(final Method method, final Object target,
                          final Object[] args, final State state) {
            this.method = method;
            this.target = target;
            this.args = args;
            this.state = state;
        }
    }

    private static final int QUEUED_CALLS_COUNT = 16;
    private static final ArrayList<QueuedCall> QUEUED_CALLS = new ArrayList<>(QUEUED_CALLS_COUNT);

    private static final Runnable UI_THREAD_CALLBACK = new Runnable() {
        @Override
        public void run() {
            ThreadUtils.assertOnUiThread();
            long nextDelay = runUiThreadCallback();
            if (nextDelay >= 0) {
                ThreadUtils.getUiHandler().postDelayed(this, nextDelay);
            }
        }
    };

    private static GoannaThread sGoannaThread;

    @WrapForJNI
    private static final ClassLoader clsLoader = GoannaThread.class.getClassLoader();
    @WrapForJNI
    private static MessageQueue msgQueue;

    private GoannaProfile mProfile;

    private final String mArgs;
    private final String mAction;
    private final boolean mDebugging;

    private String[] mChildProcessArgs;
    private int mCrashFileDescriptor;
    private int mIPCFileDescriptor;

    GoannaThread(GoannaProfile profile, String args, String action, boolean debugging) {
        mProfile = profile;
        mArgs = args;
        mAction = action;
        mDebugging = debugging;
        mChildProcessArgs = null;
        mCrashFileDescriptor = -1;
        mIPCFileDescriptor = -1;

        setName("Goanna");
    }

    public static boolean init(GoannaProfile profile, String args, String action, boolean debugging) {
        ThreadUtils.assertOnUiThread();
        if (isState(State.INITIAL) && sGoannaThread == null) {
            sGoannaThread = new GoannaThread(profile, args, action, debugging);
            return true;
        }
        return false;
    }

    public static boolean initChildProcess(GoannaProfile profile, String[] args, int crashFd, int ipcFd, boolean debugging) {
        if (init(profile, null, null, debugging)) {
            sGoannaThread.mChildProcessArgs = args;
            sGoannaThread.mCrashFileDescriptor = crashFd;
            sGoannaThread.mIPCFileDescriptor = ipcFd;
            return true;
        }
        return false;
    }

    private static boolean canUseProfile(final Context context, final GoannaProfile profile,
                                         final String profileName, final File profileDir) {
        if (profileDir != null && !profileDir.isDirectory()) {
            return false;
        }

        if (profile == null) {
            // We haven't initialized; any profile is okay as long as we follow the guest mode setting.
            return GoannaProfile.shouldUseGuestMode(context) ==
                    GoannaProfile.isGuestProfile(context, profileName, profileDir);
        }

        // We already initialized and have a profile; see if it matches ours.
        try {
            return profileDir == null ? profileName.equals(profile.getName()) :
                    profile.getDir().getCanonicalPath().equals(profileDir.getCanonicalPath());
        } catch (final IOException e) {
            Log.e(LOGTAG, "Cannot compare profile " + profileName);
            return false;
        }
    }

    public static boolean canUseProfile(final String profileName, final File profileDir) {
        if (profileName == null) {
            throw new IllegalArgumentException("Null profile name");
        }
        return canUseProfile(GoannaAppShell.getApplicationContext(), getActiveProfile(),
                             profileName, profileDir);
    }

    public static boolean initWithProfile(final String profileName, final File profileDir) {
        if (profileName == null) {
            throw new IllegalArgumentException("Null profile name");
        }

        final Context context = GoannaAppShell.getApplicationContext();
        final GoannaProfile profile = getActiveProfile();

        if (!canUseProfile(context, profile, profileName, profileDir)) {
            // Profile is incompatible with current profile.
            return false;
        }

        if (profile != null) {
            // We already have a compatible profile.
            return true;
        }

        // We haven't initialized yet; okay to initialize now.
        return init(GoannaProfile.get(context, profileName, profileDir),
                    /* args */ null, /* action */ null, /* debugging */ false);
    }

    public static boolean launch() {
        ThreadUtils.assertOnUiThread();
        if (checkAndSetState(State.INITIAL, State.LAUNCHED)) {
            sGoannaThread.start();
            return true;
        }
        return false;
    }

    public static boolean isLaunched() {
        return !isState(State.INITIAL);
    }

    @RobocopTarget
    public static boolean isRunning() {
        return isState(State.RUNNING);
    }

    // Invoke the given Method and handle checked Exceptions.
    private static void invokeMethod(final Method method, final Object obj, final Object[] args) {
        try {
            method.setAccessible(true);
            method.invoke(obj, args);
        } catch (final IllegalAccessException e) {
            throw new IllegalStateException("Unexpected exception", e);
        } catch (final InvocationTargetException e) {
            throw new UnsupportedOperationException("Cannot make call", e.getCause());
        }
    }

    // Queue a call to the given method.
    private static void queueNativeCallLocked(final Class<?> cls, final String methodName,
                                              final Object obj, final Object[] args,
                                              final State state) {
        final ArrayList<Class<?>> argTypes = new ArrayList<>(args.length);
        final ArrayList<Object> argValues = new ArrayList<>(args.length);

        for (int i = 0; i < args.length; i++) {
            if (args[i] instanceof Class) {
                argTypes.add((Class<?>) args[i]);
                argValues.add(args[++i]);
                continue;
            }
            Class<?> argType = args[i].getClass();
            if (argType == Boolean.class) argType = Boolean.TYPE;
            else if (argType == Byte.class) argType = Byte.TYPE;
            else if (argType == Character.class) argType = Character.TYPE;
            else if (argType == Double.class) argType = Double.TYPE;
            else if (argType == Float.class) argType = Float.TYPE;
            else if (argType == Integer.class) argType = Integer.TYPE;
            else if (argType == Long.class) argType = Long.TYPE;
            else if (argType == Short.class) argType = Short.TYPE;
            argTypes.add(argType);
            argValues.add(args[i]);
        }
        final Method method;
        try {
            method = cls.getDeclaredMethod(
                    methodName, argTypes.toArray(new Class<?>[argTypes.size()]));
        } catch (final NoSuchMethodException e) {
            throw new IllegalArgumentException("Cannot find method", e);
        }

        if (!Modifier.isNative(method.getModifiers())) {
            // As a precaution, we disallow queuing non-native methods. Queuing non-native
            // methods is dangerous because the method could end up being called on either
            // the original thread or the Goanna thread depending on timing. Native methods
            // usually handle this by posting an event to the Goanna thread automatically,
            // but there is no automatic mechanism for non-native methods.
            throw new UnsupportedOperationException("Not allowed to queue non-native methods");
        }

        if (isStateAtLeast(state)) {
            invokeMethod(method, obj, argValues.toArray());
            return;
        }

        QUEUED_CALLS.add(new QueuedCall(
                method, obj, argValues.toArray(), state));
    }

    /**
     * Queue a call to the given static method until Goanna is in the given state.
     *
     * @param state The Goanna state in which the native call could be executed.
     *              Default is State.RUNNING, which means this queued call will
     *              run when Goanna is at or after RUNNING state.
     * @param cls Class that declares the static method.
     * @param methodName Name of the static method.
     * @param args Args to call the static method with; to specify a parameter type,
     *             pass in a Class instance first, followed by the value.
     */
    public static void queueNativeCallUntil(final State state, final Class<?> cls,
                                            final String methodName, final Object... args) {
        synchronized (QUEUED_CALLS) {
            queueNativeCallLocked(cls, methodName, null, args, state);
        }
    }

    /**
     * Queue a call to the given static method until Goanna is in the RUNNING state.
     */
    public static void queueNativeCall(final Class<?> cls, final String methodName,
                                       final Object... args) {
        synchronized (QUEUED_CALLS) {
            queueNativeCallLocked(cls, methodName, null, args, State.RUNNING);
        }
    }

    /**
     * Queue a call to the given instance method until Goanna is in the given state.
     *
     * @param state The Goanna state in which the native call could be executed.
     * @param obj Object that declares the instance method.
     * @param methodName Name of the instance method.
     * @param args Args to call the instance method with; to specify a parameter type,
     *             pass in a Class instance first, followed by the value.
     */
    public static void queueNativeCallUntil(final State state, final Object obj,
                                            final String methodName, final Object... args) {
        synchronized (QUEUED_CALLS) {
            queueNativeCallLocked(obj.getClass(), methodName, obj, args, state);
        }
    }

    /**
     * Queue a call to the given instance method until Goanna is in the RUNNING state.
     */
    public static void queueNativeCall(final Object obj, final String methodName,
                                       final Object... args) {
        synchronized (QUEUED_CALLS) {
            queueNativeCallLocked(obj.getClass(), methodName, obj, args, State.RUNNING);
        }
    }

    // Run all queued methods
    private static void flushQueuedNativeCallsLocked(final State state) {
        int lastSkipped = -1;
        for (int i = 0; i < QUEUED_CALLS.size(); i++) {
            final QueuedCall call = QUEUED_CALLS.get(i);
            if (call == null) {
                // We already handled the call.
                continue;
            }
            if (!state.isAtLeast(call.state)) {
                // The call is not ready yet; skip it.
                lastSkipped = i;
                continue;
            }
            // Mark as handled.
            QUEUED_CALLS.set(i, null);

            invokeMethod(call.method, call.target, call.args);
        }
        if (lastSkipped < 0) {
            // We're done here; release the memory
            QUEUED_CALLS.clear();
            QUEUED_CALLS.trimToSize();
        } else if (lastSkipped < QUEUED_CALLS.size() - 1) {
            // We skipped some; free up null entries at the end,
            // but keep all the previous entries for later.
            QUEUED_CALLS.subList(lastSkipped + 1, QUEUED_CALLS.size()).clear();
        }
    }

    private static void loadGoannaLibs(final Context context, final String resourcePath) {
        GoannaLoader.loadSQLiteLibs(context, resourcePath);
        GoannaLoader.loadNSSLibs(context, resourcePath);
        GoannaLoader.loadGoannaLibs(context, resourcePath);
    }

    private static String initGoannaEnvironment() {
        final Context context = GoannaAppShell.getApplicationContext();
        GoannaLoader.loadMozGlue(context);
        setState(State.MOZGLUE_READY);

        final Locale locale = Locale.getDefault();
        final Resources res = context.getResources();
        if (locale.toString().equalsIgnoreCase("zh_hk")) {
            final Locale mappedLocale = Locale.TRADITIONAL_CHINESE;
            Locale.setDefault(mappedLocale);
            Configuration config = res.getConfiguration();
            config.locale = mappedLocale;
            res.updateConfiguration(config, null);
        }

        String[] pluginDirs = null;
        try {
            pluginDirs = GoannaAppShell.getPluginDirectories();
        } catch (Exception e) {
            Log.w(LOGTAG, "Caught exception getting plugin dirs.", e);
        }

        final String resourcePath = context.getPackageResourcePath();
        GoannaLoader.setupGoannaEnvironment(context, pluginDirs, context.getFilesDir().getPath());

        try {
            loadGoannaLibs(context, resourcePath);

        } catch (final Exception e) {
            // Cannot load libs; try clearing the cached files.
            Log.w(LOGTAG, "Clearing cache after load libs exception", e);
            FileUtils.delTree(GoannaLoader.getCacheDir(context),
                              new FileUtils.FilenameRegexFilter(".*\\.so(?:\\.crc)?$"),
                              /* recurse */ true);

            // Then try loading again. If this throws again, we actually crash.
            loadGoannaLibs(context, resourcePath);
        }

        setState(State.LIBS_READY);
        return resourcePath;
    }

    private void addCustomProfileArg(String args, ArrayList<String> list) {
        // Make sure a profile exists.
        final GoannaProfile profile = getProfile();
        profile.getDir(); // call the lazy initializer

        boolean needsProfile = true;

        if (args != null) {
            StringTokenizer st = new StringTokenizer(args);
            while (st.hasMoreTokens()) {
                String token = st.nextToken();
                if ("-P".equals(token) || "-profile".equals(token)) {
                    needsProfile = false;
                }
                list.add(token);
            }
        }

        // If args don't include the profile, make sure it's included.
        if (args == null || needsProfile) {
            if (profile.isCustomProfile()) {
                list.add("-profile");
                list.add(profile.getDir().getAbsolutePath());
            } else {
                list.add("-P");
                list.add(profile.getName());
            }
        }
    }

    private String[] getGoannaArgs(final String apkPath) {
        // argv[0] is the program name, which for us is the package name.
        final Context context = GoannaAppShell.getApplicationContext();
        final ArrayList<String> args = new ArrayList<String>();
        args.add(context.getPackageName());
        args.add("-greomni");
        args.add(apkPath);

        addCustomProfileArg(mArgs, args);

        // In un-official builds, we want to load Javascript resources fresh
        // with each build.  In official builds, the startup cache is purged by
        // the buildid mechanism, but most un-official builds don't bump the
        // buildid, so we purge here instead.
        final GoannaAppShell.GoannaInterface gi = GoannaAppShell.getGoannaInterface();
        if (gi == null || !gi.isOfficial()) {
            Log.w(LOGTAG, "STARTUP PERFORMANCE WARNING: un-official build: purging the " +
                          "startup (JavaScript) caches.");
            args.add("-purgecaches");
        }

        return args.toArray(new String[args.size()]);
    }

    public static GoannaProfile getActiveProfile() {
        if (sGoannaThread == null) {
            return null;
        }
        final GoannaProfile profile = sGoannaThread.mProfile;
        if (profile != null) {
            return profile;
        }
        return sGoannaThread.getProfile();
    }

    public synchronized GoannaProfile getProfile() {
        if (mProfile == null) {
            final Context context = GoannaAppShell.getApplicationContext();
            mProfile = GoannaProfile.initFromArgs(context, mArgs);
        }
        return mProfile;
    }

    @Override
    public void run() {
        Log.i(LOGTAG, "preparing to run Goanna");

        Looper.prepare();
        GoannaThread.msgQueue = Looper.myQueue();
        ThreadUtils.sGoannaThread = this;
        ThreadUtils.sGoannaHandler = new Handler();

        // Preparation for pumpMessageLoop()
        final MessageQueue.IdleHandler idleHandler = new MessageQueue.IdleHandler() {
            @Override public boolean queueIdle() {
                final Handler goannaHandler = ThreadUtils.sGoannaHandler;
                Message idleMsg = Message.obtain(goannaHandler);
                // Use |Message.obj == GoannaHandler| to identify our "queue is empty" message
                idleMsg.obj = goannaHandler;
                goannaHandler.sendMessageAtFrontOfQueue(idleMsg);
                // Keep this IdleHandler
                return true;
            }
        };
        Looper.myQueue().addIdleHandler(idleHandler);

        if (mDebugging) {
            try {
                Thread.sleep(5 * 1000 /* 5 seconds */);
            } catch (final InterruptedException e) {
            }
        }

        final String[] args;
        if (mChildProcessArgs != null) {
            initGoannaEnvironment();
            args = mChildProcessArgs;
        } else {
            args = getGoannaArgs(initGoannaEnvironment());
        }

        // This can only happen after the call to initGoannaEnvironment
        // above, because otherwise the JNI code hasn't been loaded yet.
        ThreadUtils.postToUiThread(new Runnable() {
            @Override public void run() {
                registerUiThread();
            }
        });

        Log.w(LOGTAG, "zerdatime " + SystemClock.uptimeMillis() + " - runGoanna");

        final GoannaAppShell.GoannaInterface gi = GoannaAppShell.getGoannaInterface();
        if (gi == null || !gi.isOfficial()) {
            Log.i(LOGTAG, "RunGoanna - args = " + args);
        }

        // And go.
        GoannaLoader.nativeRun(args, mCrashFileDescriptor, mIPCFileDescriptor);

        // And... we're done.
        setState(State.EXITED);

        EventDispatcher.getInstance().dispatch("Goanna:Exited", null);

        // Remove pumpMessageLoop() idle handler
        Looper.myQueue().removeIdleHandler(idleHandler);
    }

    @WrapForJNI(calledFrom = "goanna")
    private static boolean pumpMessageLoop(final Message msg) {
        final Handler goannaHandler = ThreadUtils.sGoannaHandler;

        if (msg.obj == goannaHandler && msg.getTarget() == goannaHandler) {
            // Our "queue is empty" message; see runGoanna()
            return false;
        }

        if (msg.getTarget() == null) {
            Looper.myLooper().quit();
        } else {
            msg.getTarget().dispatchMessage(msg);
        }

        return true;
    }

    /**
     * Check that the current Goanna thread state matches the given state.
     *
     * @param state State to check
     * @return True if the current Goanna thread state matches
     */
    public static boolean isState(final State state) {
        return sState.is(state);
    }

    /**
     * Check that the current Goanna thread state is at the given state or further along,
     * according to the order defined in the State enum.
     *
     * @param state State to check
     * @return True if the current Goanna thread state matches
     */
    public static boolean isStateAtLeast(final State state) {
        return sState.isAtLeast(state);
    }

    /**
     * Check that the current Goanna thread state is at the given state or prior,
     * according to the order defined in the State enum.
     *
     * @param state State to check
     * @return True if the current Goanna thread state matches
     */
    public static boolean isStateAtMost(final State state) {
        return sState.isAtMost(state);
    }

    /**
     * Check that the current Goanna thread state falls into an inclusive range of states,
     * according to the order defined in the State enum.
     *
     * @param minState Lower range of allowable states
     * @param maxState Upper range of allowable states
     * @return True if the current Goanna thread state matches
     */
    public static boolean isStateBetween(final State minState, final State maxState) {
        return sState.isBetween(minState, maxState);
    }

    @WrapForJNI(calledFrom = "goanna")
    private static void setState(final State newState) {
        ThreadUtils.assertOnGoannaThread();
        synchronized (QUEUED_CALLS) {
            flushQueuedNativeCallsLocked(newState);
            sState = newState;
        }
    }

    @WrapForJNI(calledFrom = "goanna")
    private static boolean checkAndSetState(final State currentState, final State newState) {
        synchronized (QUEUED_CALLS) {
            if (sState == currentState) {
                flushQueuedNativeCallsLocked(newState);
                sState = newState;
                return true;
            }
        }
        return false;
    }

    @WrapForJNI(stubName = "SpeculativeConnect")
    private static native void speculativeConnectNative(String uri);

    public static void speculativeConnect(final String uri) {
        // This is almost always called before Goanna loads, so we don't
        // bother checking here if Goanna is actually loaded or not.
        // Speculative connection depends on proxy settings,
        // so the earliest it can happen is after profile is ready.
        queueNativeCallUntil(State.PROFILE_READY, GoannaThread.class,
                             "speculativeConnectNative", uri);
    }

    @WrapForJNI @RobocopTarget
    public static native void waitOnGoanna();

    @WrapForJNI(stubName = "OnPause", dispatchTo = "goanna")
    private static native void nativeOnPause();

    public static void onPause() {
        if (isStateAtLeast(State.PROFILE_READY)) {
            nativeOnPause();
        } else {
            queueNativeCallUntil(State.PROFILE_READY, GoannaThread.class,
                                 "nativeOnPause");
        }
    }

    @WrapForJNI(stubName = "OnResume", dispatchTo = "goanna")
    private static native void nativeOnResume();

    public static void onResume() {
        if (isStateAtLeast(State.PROFILE_READY)) {
            nativeOnResume();
        } else {
            queueNativeCallUntil(State.PROFILE_READY, GoannaThread.class,
                                 "nativeOnResume");
        }
    }

    @WrapForJNI(stubName = "CreateServices", dispatchTo = "goanna")
    private static native void nativeCreateServices(String category, String data);

    public static void createServices(final String category, final String data) {
        if (isStateAtLeast(State.PROFILE_READY)) {
            nativeCreateServices(category, data);
        } else {
            queueNativeCallUntil(State.PROFILE_READY, GoannaThread.class, "nativeCreateServices",
                                 String.class, category, String.class, data);
        }
    }

    // Implemented in mozglue/android/APKOpen.cpp.
    /* package */ static native void registerUiThread();

    @WrapForJNI(calledFrom = "ui")
    /* package */ static native long runUiThreadCallback();

    @WrapForJNI
    private static void requestUiThreadCallback(long delay) {
        ThreadUtils.getUiHandler().postDelayed(UI_THREAD_CALLBACK, delay);
    }
}
