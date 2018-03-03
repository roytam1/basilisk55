/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna;

import org.mozilla.goanna.menu.GoannaMenu;
import org.mozilla.goanna.menu.GoannaMenuItem;
import org.mozilla.goanna.util.ResourceDrawableUtils;
import org.mozilla.goanna.text.TextSelection;
import org.mozilla.goanna.util.BundleEventListener;
import org.mozilla.goanna.util.EventCallback;
import org.mozilla.goanna.util.GoannaBundle;
import org.mozilla.goanna.util.ThreadUtils;
import org.mozilla.goanna.ActionModeCompat.Callback;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.view.MenuItem;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.Arrays;
import java.util.Timer;
import java.util.TimerTask;

import android.util.Log;

class ActionBarTextSelection implements TextSelection, BundleEventListener {
    private static final String LOGTAG = "GoannaTextSelection";
    private static final int SHUTDOWN_DELAY_MS = 250;

    private final Context context;

    private boolean mDraggingHandles;

    private int selectionID; // Unique ID provided for each selection action.

    private GoannaBundle[] mCurrentItems;

    private TextSelectionActionModeCallback mCallback;

    // These timers are used to avoid flicker caused by selection handles showing/hiding quickly.
    // For instance when moving between single handle caret mode and two handle selection mode.
    private final Timer mActionModeTimer = new Timer("actionMode");
    private class ActionModeTimerTask extends TimerTask {
        @Override
        public void run() {
            ThreadUtils.postToUiThread(new Runnable() {
                @Override
                public void run() {
                    endActionMode();
                }
            });
        }
    };
    private ActionModeTimerTask mActionModeTimerTask;

    ActionBarTextSelection(Context context) {
        this.context = context;
    }

    @Override
    public void create() {
        // Only register listeners if we have valid start/middle/end handles
        if (context == null) {
            Log.e(LOGTAG, "Failed to initialize text selection because at least one context is null");
        } else {
            GoannaApp.getEventDispatcher().registerUiThreadListener(this,
                    "TextSelection:ActionbarInit",
                    "TextSelection:ActionbarStatus",
                    "TextSelection:ActionbarUninit");
        }
    }

    @Override
    public boolean dismiss() {
        // We do not call endActionMode() here because this is already handled by the activity.
        return false;
    }

    @Override
    public void destroy() {
        if (context == null) {
            Log.e(LOGTAG, "Do not unregister TextSelection:* listeners since context is null");
        } else {
            GoannaApp.getEventDispatcher().unregisterUiThreadListener(this,
                    "TextSelection:ActionbarInit",
                    "TextSelection:ActionbarStatus",
                    "TextSelection:ActionbarUninit");
        }
    }

    @Override
    public void handleMessage(final String event, final GoannaBundle message,
                              final EventCallback callback) {
        if ("TextSelection:ActionbarInit".equals(event)) {
            // Init / Open the action bar. Note the current selectionID,
            // cancel any pending actionBar close.
            Telemetry.sendUIEvent(TelemetryContract.Event.SHOW,
                TelemetryContract.Method.CONTENT, "text_selection");

            selectionID = message.getInt("selectionID");
            mCurrentItems = null;
            if (mActionModeTimerTask != null) {
                mActionModeTimerTask.cancel();
            }

        } else if ("TextSelection:ActionbarStatus".equals(event)) {
            // Ensure async updates from SearchService for example are valid.
            if (selectionID != message.getInt("selectionID")) {
                return;
            }

            // Update the actionBar actions as provided by Goanna.
            showActionMode(message.getBundleArray("actions"));

        } else if ("TextSelection:ActionbarUninit".equals(event)) {
            // Uninit the actionbar. Schedule a cancellable close
            // action to avoid UI jank. (During SelectionAll for ex).
            mCurrentItems = null;
            mActionModeTimerTask = new ActionModeTimerTask();
            mActionModeTimer.schedule(mActionModeTimerTask, SHUTDOWN_DELAY_MS);
        }
    }

    private void showActionMode(final GoannaBundle[] items) {
        if (Arrays.equals(items, mCurrentItems)) {
            return;
        }
        mCurrentItems = items;

        if (mCallback != null) {
            mCallback.updateItems(items);
            return;
        }

        if (context instanceof ActionModeCompat.Presenter) {
            final ActionModeCompat.Presenter presenter = (ActionModeCompat.Presenter) context;
            mCallback = new TextSelectionActionModeCallback(items);
            presenter.startActionModeCompat(mCallback);
            mCallback.animateIn();
        }
    }

    private void endActionMode() {
        if (context instanceof ActionModeCompat.Presenter) {
            final ActionModeCompat.Presenter presenter = (ActionModeCompat.Presenter) context;
            presenter.endActionModeCompat();
        }
        mCurrentItems = null;
    }

    private class TextSelectionActionModeCallback implements Callback {
        private GoannaBundle[] mItems;
        private ActionModeCompat mActionMode;

        public TextSelectionActionModeCallback(final GoannaBundle[] items) {
            mItems = items;
        }

        public void updateItems(final GoannaBundle[] items) {
            mItems = items;
            if (mActionMode != null) {
                mActionMode.invalidate();
            }
        }

        public void animateIn() {
            if (mActionMode != null) {
                mActionMode.animateIn();
            }
        }

        @Override
        public boolean onPrepareActionMode(final ActionModeCompat mode, final GoannaMenu menu) {
            // Android would normally expect us to only update the state of menu items
            // here To make the js-java interaction a bit simpler, we just wipe out the
            // menu here and recreate all the javascript menu items in onPrepare instead.
            // This will be called any time invalidate() is called on the action mode.
            menu.clear();

            final int length = mItems.length;
            for (int i = 0; i < length; i++) {
                final GoannaBundle obj = mItems[i];
                final GoannaMenuItem menuitem = (GoannaMenuItem)
                        menu.add(0, i, 0, obj.getString("label"));
                final int actionEnum = obj.getBoolean("showAsAction") ?
                        GoannaMenuItem.SHOW_AS_ACTION_ALWAYS : GoannaMenuItem.SHOW_AS_ACTION_NEVER;
                menuitem.setShowAsAction(actionEnum, R.attr.menuItemActionModeStyle);

                final String iconString = obj.getString("icon");
                ResourceDrawableUtils.getDrawable(context, iconString,
                        new ResourceDrawableUtils.BitmapLoader() {
                    @Override
                    public void onBitmapFound(Drawable d) {
                        if (d != null) {
                            menuitem.setIcon(d);
                        }
                    }
                });
            }
            return true;
        }

        @Override
        public boolean onCreateActionMode(ActionModeCompat mode, GoannaMenu unused) {
            mActionMode = mode;
            return true;
        }

        @Override
        public boolean onActionItemClicked(ActionModeCompat mode, MenuItem item) {
            final GoannaBundle obj = mItems[item.getItemId()];
            GoannaAppShell.notifyObservers("TextSelection:Action", obj.getString("id"));
            return true;
        }

        // Called when the user exits the action mode
        @Override
        public void onDestroyActionMode(ActionModeCompat mode) {
            mActionMode = null;
            mCallback = null;
            final JSONObject args = new JSONObject();
            try {
                args.put("selectionID", selectionID);
            } catch (JSONException e) {
                Log.e(LOGTAG, "Error building JSON arguments for TextSelection:End", e);
                return;
            }

            GoannaAppShell.notifyObservers("TextSelection:End", args.toString());
        }
    }
}
