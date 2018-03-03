/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna;

import android.support.v4.app.Fragment;
import android.os.Bundle;
import android.os.Parcelable;
import android.util.Log;
import android.util.SparseArray;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

public class GoannaViewFragment extends android.support.v4.app.Fragment {
    private static final String LOGTAG = "GoannaViewFragment";

    private static Parcelable state = null;
    private static GoannaViewFragment lastUsed = null;
    private GoannaView goannaView = null;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        setRetainInstance(true);
        super.onCreate(savedInstanceState);
    }

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        goannaView = new GoannaView(getContext());
        return goannaView;
    }

    @Override
    public void onResume() {
        if (state != null && lastUsed != this) {
            // "Restore" the window from the previously used GoannaView to this GoannaView and attach it
            goannaView.onRestoreInstanceState(state);
            state = null;
        }
        super.onResume();
    }

    @Override
    public void onPause() {
        state = goannaView.onSaveInstanceState();
        lastUsed = this;
        super.onPause();
    }
}
