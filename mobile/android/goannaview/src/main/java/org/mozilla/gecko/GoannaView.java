/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: ts=4 sw=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna;

import java.util.Set;

import org.json.JSONException;
import org.json.JSONObject;
import org.mozilla.goanna.annotation.ReflectionTarget;
import org.mozilla.goanna.annotation.WrapForJNI;
import org.mozilla.goanna.gfx.LayerView;
import org.mozilla.goanna.mozglue.JNIObject;
import org.mozilla.goanna.util.EventCallback;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.AttributeSet;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

public class GoannaView extends LayerView
    implements ContextGetter {

    private static final String DEFAULT_SHARED_PREFERENCES_FILE = "GoannaView";
    private static final String LOGTAG = "GoannaView";

    private final EventDispatcher eventDispatcher = new EventDispatcher();

    private ChromeDelegate mChromeDelegate;
    private ContentDelegate mContentDelegate;

    private InputConnectionListener mInputConnectionListener;

    protected boolean onAttachedToWindowCalled;
    protected String chromeURI;
    protected int screenId = 0; // default to the primary screen

    @WrapForJNI(dispatchTo = "proxy")
    protected static final class Window extends JNIObject {
        @WrapForJNI(skip = true)
        /* package */ Window() {}

        static native void open(Window instance, GoannaView view, Object compositor,
                                EventDispatcher dispatcher, String chromeURI, int screenId);

        @Override protected native void disposeNative();
        native void close();
        native void reattach(GoannaView view, Object compositor, EventDispatcher dispatcher);
        native void loadUri(String uri, int flags);
    }

    // Object to hold onto our nsWindow connection when GoannaView gets destroyed.
    private static class StateBinder extends Binder implements Parcelable {
        public final Parcelable superState;
        public final Window window;

        public StateBinder(Parcelable superState, Window window) {
            this.superState = superState;
            this.window = window;
        }

        @Override
        public int describeContents() {
            return 0;
        }

        @Override
        public void writeToParcel(Parcel out, int flags) {
            // Always write out the super-state, so that even if we lose this binder, we
            // will still have something to pass into super.onRestoreInstanceState.
            out.writeParcelable(superState, flags);
            out.writeStrongBinder(this);
        }

        @ReflectionTarget
        public static final Parcelable.Creator<StateBinder> CREATOR
            = new Parcelable.Creator<StateBinder>() {
                @Override
                public StateBinder createFromParcel(Parcel in) {
                    final Parcelable superState = in.readParcelable(null);
                    final IBinder binder = in.readStrongBinder();
                    if (binder instanceof StateBinder) {
                        return (StateBinder) binder;
                    }
                    // Not the original object we saved; return null state.
                    return new StateBinder(superState, null);
                }

                @Override
                public StateBinder[] newArray(int size) {
                    return new StateBinder[size];
                }
            };
    }

    protected Window window;
    private boolean stateSaved;

    public GoannaView(Context context) {
        super(context);
        init(context);
    }

    public GoannaView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    private void init(Context context) {
        if (GoannaAppShell.getApplicationContext() == null) {
            GoannaAppShell.setApplicationContext(context.getApplicationContext());
        }

        // Set the GoannaInterface if the context is an activity and the GoannaInterface
        // has not already been set
        if (context instanceof Activity && getGoannaInterface() == null) {
            setGoannaInterface(new BaseGoannaInterface(context));
            GoannaAppShell.setContextGetter(this);
        }

        // Perform common initialization for Fennec/GoannaView.
        GoannaAppShell.setLayerView(this);

        initializeView();
    }

    @Override
    protected Parcelable onSaveInstanceState()
    {
        final Parcelable superState = super.onSaveInstanceState();
        stateSaved = true;
        return new StateBinder(superState, this.window);
    }

    @Override
    protected void onRestoreInstanceState(final Parcelable state)
    {
        final StateBinder stateBinder = (StateBinder) state;

        if (stateBinder.window != null) {
            this.window = stateBinder.window;
        }
        stateSaved = false;

        if (onAttachedToWindowCalled) {
            reattachWindow();
        }

        // We have to always call super.onRestoreInstanceState because View keeps
        // track of these calls and throws an exception when we don't call it.
        super.onRestoreInstanceState(stateBinder.superState);
    }

    protected void openWindow() {
        if (chromeURI == null) {
            chromeURI = getGoannaInterface().getDefaultChromeURI();
        }

        if (GoannaThread.isStateAtLeast(GoannaThread.State.PROFILE_READY)) {
            Window.open(window, this, getCompositor(), eventDispatcher,
                        chromeURI, screenId);
        } else {
            GoannaThread.queueNativeCallUntil(GoannaThread.State.PROFILE_READY, Window.class,
                    "open", window, GoannaView.class, this, Object.class, getCompositor(),
                    EventDispatcher.class, eventDispatcher,
                    String.class, chromeURI, screenId);
        }
    }

    protected void reattachWindow() {
        if (GoannaThread.isStateAtLeast(GoannaThread.State.PROFILE_READY)) {
            window.reattach(this, getCompositor(), eventDispatcher);
        } else {
            GoannaThread.queueNativeCallUntil(GoannaThread.State.PROFILE_READY,
                    window, "reattach", GoannaView.class, this,
                    Object.class, getCompositor(), EventDispatcher.class, eventDispatcher);
        }
    }

    @Override
    public void onAttachedToWindow()
    {
        final DisplayMetrics metrics = getContext().getResources().getDisplayMetrics();

        if (window == null) {
            // Open a new nsWindow if we didn't have one from before.
            window = new Window();
            openWindow();
        } else {
            reattachWindow();
        }

        super.onAttachedToWindow();

        onAttachedToWindowCalled = true;
    }

    @Override
    public void onDetachedFromWindow()
    {
        super.onDetachedFromWindow();
        super.destroy();

        if (stateSaved) {
            // If we saved state earlier, we don't want to close the nsWindow.
            return;
        }

        if (GoannaThread.isStateAtLeast(GoannaThread.State.PROFILE_READY)) {
            window.close();
            window.disposeNative();
        } else {
            GoannaThread.queueNativeCallUntil(GoannaThread.State.PROFILE_READY,
                    window, "close");
            GoannaThread.queueNativeCallUntil(GoannaThread.State.PROFILE_READY,
                    window, "disposeNative");
        }

        onAttachedToWindowCalled = false;
    }

    @WrapForJNI public static final int LOAD_DEFAULT = 0;
    @WrapForJNI public static final int LOAD_NEW_TAB = 1;
    @WrapForJNI public static final int LOAD_SWITCH_TAB = 2;

    public void loadUri(String uri, int flags) {
        if (window == null) {
            throw new IllegalStateException("Not attached to window");
        }

        if (GoannaThread.isRunning()) {
            window.loadUri(uri, flags);
        }  else {
            GoannaThread.queueNativeCall(window, "loadUri", String.class, uri, flags);
        }
    }

    /* package */ void setInputConnectionListener(final InputConnectionListener icl) {
        mInputConnectionListener = icl;
    }

    @Override
    public Handler getHandler() {
        if (mInputConnectionListener != null) {
            return mInputConnectionListener.getHandler(super.getHandler());
        }
        return super.getHandler();
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        if (mInputConnectionListener != null) {
            return mInputConnectionListener.onCreateInputConnection(outAttrs);
        }
        return null;
    }

    @Override
    public boolean onKeyPreIme(int keyCode, KeyEvent event) {
        if (super.onKeyPreIme(keyCode, event)) {
            return true;
        }
        return mInputConnectionListener != null &&
                mInputConnectionListener.onKeyPreIme(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (super.onKeyUp(keyCode, event)) {
            return true;
        }
        return mInputConnectionListener != null &&
                mInputConnectionListener.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (super.onKeyDown(keyCode, event)) {
            return true;
        }
        return mInputConnectionListener != null &&
                mInputConnectionListener.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyLongPress(int keyCode, KeyEvent event) {
        if (super.onKeyLongPress(keyCode, event)) {
            return true;
        }
        return mInputConnectionListener != null &&
                mInputConnectionListener.onKeyLongPress(keyCode, event);
    }

    @Override
    public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
        if (super.onKeyMultiple(keyCode, repeatCount, event)) {
            return true;
        }
        return mInputConnectionListener != null &&
                mInputConnectionListener.onKeyMultiple(keyCode, repeatCount, event);
    }

    /* package */ boolean isIMEEnabled() {
        return mInputConnectionListener != null &&
                mInputConnectionListener.isIMEEnabled();
    }

    public void importScript(final String url) {
        if (url.startsWith("resource://android/assets/")) {
            GoannaAppShell.notifyObservers("GoannaView:ImportScript", url);
            return;
        }

        throw new IllegalArgumentException("Must import script from 'resources://android/assets/' location.");
    }

    /**
    * Set the chrome callback handler.
    * This will replace the current handler.
    * @param chrome An implementation of GoannaViewChrome.
    */
    public void setChromeDelegate(ChromeDelegate chrome) {
        mChromeDelegate = chrome;
    }

    /**
    * Set the content callback handler.
    * This will replace the current handler.
    * @param content An implementation of ContentDelegate.
    */
    public void setContentDelegate(ContentDelegate content) {
        mContentDelegate = content;
    }

    public static void setGoannaInterface(final BaseGoannaInterface goannaInterface) {
        GoannaAppShell.setGoannaInterface(goannaInterface);
    }

    public static GoannaAppShell.GoannaInterface getGoannaInterface() {
        return GoannaAppShell.getGoannaInterface();
    }

    protected String getSharedPreferencesFile() {
        return DEFAULT_SHARED_PREFERENCES_FILE;
    }

    @Override
    public SharedPreferences getSharedPreferences() {
        return getContext().getSharedPreferences(getSharedPreferencesFile(), 0);
    }

    public EventDispatcher getEventDispatcher() {
        return eventDispatcher;
    }

    /* Provides a means for the client to indicate whether a JavaScript
     * dialog request should proceed. An instance of this class is passed to
     * various GoannaViewChrome callback actions.
     */
    public class PromptResult {
        private final int RESULT_OK = 0;
        private final int RESULT_CANCEL = 1;

        private final JSONObject mMessage;

        public PromptResult(JSONObject message) {
            mMessage = message;
        }

        private JSONObject makeResult(int resultCode) {
            JSONObject result = new JSONObject();
            try {
                result.put("button", resultCode);
            } catch (JSONException ex) { }
            return result;
        }

        /**
        * Handle a confirmation response from the user.
        */
        public void confirm() {
            JSONObject result = makeResult(RESULT_OK);
            EventDispatcher.sendResponse(mMessage, result);
        }

        /**
        * Handle a confirmation response from the user.
        * @param value String value to return to the browser context.
        */
        public void confirmWithValue(String value) {
            JSONObject result = makeResult(RESULT_OK);
            try {
                result.put("textbox0", value);
            } catch (JSONException ex) { }
            EventDispatcher.sendResponse(mMessage, result);
        }

        /**
        * Handle a cancellation response from the user.
        */
        public void cancel() {
            JSONObject result = makeResult(RESULT_CANCEL);
            EventDispatcher.sendResponse(mMessage, result);
        }
    }

    public interface ChromeDelegate {
        /**
        * Tell the host application to display an alert dialog.
        * @param view The GoannaView that initiated the callback.
        * @param message The string to display in the dialog.
        * @param result A PromptResult used to send back the result without blocking.
        * Defaults to cancel requests.
        */
        public void onAlert(GoannaView view, String message, GoannaView.PromptResult result);

        /**
        * Tell the host application to display a confirmation dialog.
        * @param view The GoannaView that initiated the callback.
        * @param message The string to display in the dialog.
        * @param result A PromptResult used to send back the result without blocking.
        * Defaults to cancel requests.
        */
        public void onConfirm(GoannaView view, String message, GoannaView.PromptResult result);

        /**
        * Tell the host application to display an input prompt dialog.
        * @param view The GoannaView that initiated the callback.
        * @param message The string to display in the dialog.
        * @param defaultValue The string to use as default input.
        * @param result A PromptResult used to send back the result without blocking.
        * Defaults to cancel requests.
        */
        public void onPrompt(GoannaView view, String message, String defaultValue, GoannaView.PromptResult result);

        /**
        * Tell the host application to display a remote debugging request dialog.
        * @param view The GoannaView that initiated the callback.
        * @param result A PromptResult used to send back the result without blocking.
        * Defaults to cancel requests.
        */
        public void onDebugRequest(GoannaView view, GoannaView.PromptResult result);
    }

    public interface ContentDelegate {
        /**
        * A View has started loading content from the network.
        * @param view The GoannaView that initiated the callback.
        * @param url The resource being loaded.
        */
        public void onPageStart(GoannaView view, String url);

        /**
        * A View has finished loading content from the network.
        * @param view The GoannaView that initiated the callback.
        * @param success Whether the page loaded successfully or an error occurred.
        */
        public void onPageStop(GoannaView view, boolean success);

        /**
        * A View is displaying content. This page could have been loaded via
        * network or from the session history.
        * @param view The GoannaView that initiated the callback.
        */
        public void onPageShow(GoannaView view);

        /**
        * A page title was discovered in the content or updated after the content
        * loaded.
        * @param view The GoannaView that initiated the callback.
        * @param title The title sent from the content.
        */
        public void onReceivedTitle(GoannaView view, String title);

        /**
        * A link element was discovered in the content or updated after the content
        * loaded that specifies a favicon.
        * @param view The GoannaView that initiated the callback.
        * @param url The href of the link element specifying the favicon.
        * @param size The maximum size specified for the favicon, or -1 for any size.
        */
        public void onReceivedFavicon(GoannaView view, String url, int size);
    }
}
