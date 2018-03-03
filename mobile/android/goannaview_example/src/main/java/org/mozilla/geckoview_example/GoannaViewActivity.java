/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goannaview_example;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

import org.mozilla.goanna.BaseGoannaInterface;
import org.mozilla.goanna.GoannaProfile;
import org.mozilla.goanna.GoannaThread;
import org.mozilla.goanna.GoannaView;

import static org.mozilla.goanna.GoannaView.setGoannaInterface;

public class GoannaViewActivity extends Activity {
    private static final String LOGTAG = "GoannaViewActivity";

    GoannaView mGoannaView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setGoannaInterface(new BaseGoannaInterface(this));

        setContentView(R.layout.goannaview_activity);

        mGoannaView = (GoannaView) findViewById(R.id.goanna_view);
        mGoannaView.setChromeDelegate(new MyGoannaViewChrome());
        mGoannaView.setContentDelegate(new MyGoannaViewContent());
    }

    @Override
    protected void onStart() {
        super.onStart();

        final GoannaProfile profile = GoannaProfile.get(getApplicationContext());

        GoannaThread.init(profile, /* args */ null, /* action */ null, /* debugging */ false);
        GoannaThread.launch();
    }

    private class MyGoannaViewChrome implements GoannaView.ChromeDelegate {
        @Override
        public void onAlert(GoannaView view, String message, GoannaView.PromptResult result) {
            Log.i(LOGTAG, "Alert!");
            result.confirm();
            Toast.makeText(getApplicationContext(), message, Toast.LENGTH_LONG).show();
        }

        @Override
        public void onConfirm(GoannaView view, String message, final GoannaView.PromptResult result) {
            Log.i(LOGTAG, "Confirm!");
            new AlertDialog.Builder(GoannaViewActivity.this)
                .setTitle("javaScript dialog")
                .setMessage(message)
                .setPositiveButton(android.R.string.ok,
                                   new DialogInterface.OnClickListener() {
                                       public void onClick(DialogInterface dialog, int which) {
                                           result.confirm();
                                       }
                                   })
                .setNegativeButton(android.R.string.cancel,
                                   new DialogInterface.OnClickListener() {
                                       public void onClick(DialogInterface dialog, int which) {
                                           result.cancel();
                                       }
                                   })
                .create()
                .show();
        }

        @Override
        public void onPrompt(GoannaView view, String message, String defaultValue, GoannaView.PromptResult result) {
            result.cancel();
        }

        @Override
        public void onDebugRequest(GoannaView view, GoannaView.PromptResult result) {
            Log.i(LOGTAG, "Remote Debug!");
            result.confirm();
        }
    }

    private class MyGoannaViewContent implements GoannaView.ContentDelegate {
        @Override
        public void onPageStart(GoannaView view, String url) {

        }

        @Override
        public void onPageStop(GoannaView view, boolean success) {

        }

        @Override
        public void onPageShow(GoannaView view) {

        }

        @Override
        public void onReceivedTitle(GoannaView view, String title) {
            Log.i(LOGTAG, "Received a title: " + title);
        }

        @Override
        public void onReceivedFavicon(GoannaView view, String url, int size) {
            Log.i(LOGTAG, "Received a favicon URL: " + url);
        }
    }
}
