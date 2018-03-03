/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
package org.mozilla.goanna.home.activitystream;

import android.content.Context;
import android.os.Bundle;
import android.support.v4.app.FragmentManager;
import android.support.v4.app.LoaderManager;
import android.util.AttributeSet;

import org.mozilla.goanna.animation.PropertyAnimator;
import org.mozilla.goanna.home.HomeBanner;
import org.mozilla.goanna.home.HomeFragment;
import org.mozilla.goanna.home.HomeScreen;

/**
 * HomeScreen implementation that displays ActivityStream.
 */
public class ActivityStreamHomeScreen
        extends ActivityStream
        implements HomeScreen {

    private boolean visible = false;

    public ActivityStreamHomeScreen(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    public boolean isVisible() {
        return visible;
    }

    @Override
    public void onToolbarFocusChange(boolean hasFocus) {

    }

    @Override
    public void showPanel(String panelId, Bundle restoreData) {

    }

    @Override
    public void setOnPanelChangeListener(OnPanelChangeListener listener) {

    }

    @Override
    public void setPanelStateChangeListener(HomeFragment.PanelStateChangeListener listener) {

    }

    @Override
    public void setBanner(HomeBanner banner) {

    }

    @Override
    public void load(LoaderManager lm, FragmentManager fm, String panelId, Bundle restoreData,
                     PropertyAnimator animator) {
        super.load(lm);
        visible = true;
    }

    @Override
    public void unload() {
        super.unload();
        visible = false;
    }
}
