/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna.prompts;

import org.mozilla.goanna.EventDispatcher;
import org.mozilla.goanna.GoannaApp;
import org.mozilla.goanna.GoannaAppShell;
import org.mozilla.goanna.util.BundleEventListener;
import org.mozilla.goanna.util.EventCallback;
import org.mozilla.goanna.util.GoannaBundle;
import org.mozilla.goanna.util.ThreadUtils;

import android.content.Context;
import android.util.Log;

public class PromptService implements BundleEventListener {
    private static final String LOGTAG = "GoannaPromptService";

    private final Context mContext;

    public PromptService(Context context) {
        GoannaApp.getEventDispatcher().registerUiThreadListener(this,
            "Prompt:Show",
            "Prompt:ShowTop");
        mContext = context;
    }

    public void destroy() {
        GoannaApp.getEventDispatcher().unregisterUiThreadListener(this,
            "Prompt:Show",
            "Prompt:ShowTop");
    }

    // BundleEventListener implementation
    @Override
    public void handleMessage(final String event, final GoannaBundle message,
                              final EventCallback callback) {
        Prompt p;
        p = new Prompt(mContext, new Prompt.PromptCallback() {
            @Override
            public void onPromptFinished(final GoannaBundle result) {
                callback.sendSuccess(result);
            }
        });
        p.show(message);
    }
}
