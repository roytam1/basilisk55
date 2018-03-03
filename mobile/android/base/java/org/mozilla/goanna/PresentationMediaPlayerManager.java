/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna;

import android.annotation.TargetApi;
import android.app.Presentation;
import android.content.Context;
import android.os.Bundle;
import android.support.v7.media.MediaRouter;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.mozilla.goanna.AppConstants.Versions;

import org.mozilla.goanna.annotation.WrapForJNI;

/**
 * A MediaPlayerManager with API 17+ Presentation support.
 */
@TargetApi(17)
public class PresentationMediaPlayerManager extends MediaPlayerManager {

    private static final String LOGTAG = "Goanna" + PresentationMediaPlayerManager.class.getSimpleName();

    private GoannaPresentation presentation;

    public PresentationMediaPlayerManager() {
        if (!Versions.feature17Plus) {
            throw new IllegalStateException(PresentationMediaPlayerManager.class.getSimpleName() +
                    " does not support < API 17");
        }
    }

    @Override
    public void onStop() {
        super.onStop();
        if (presentation != null) {
            presentation.dismiss();
            presentation = null;
        }
    }

    @Override
    protected void updatePresentation() {
        if (mediaRouter == null) {
            return;
        }

        if (isPresentationMode) {
            return;
        }

        MediaRouter.RouteInfo route = mediaRouter.getSelectedRoute();
        Display display = route != null ? route.getPresentationDisplay() : null;

        if (display != null) {
            if ((presentation != null) && (presentation.getDisplay() != display)) {
                presentation.dismiss();
                presentation = null;
            }

            if (presentation == null) {
                final GoannaView goannaView = (GoannaView) getActivity().findViewById(R.id.layer_view);
                presentation = new GoannaPresentation(getActivity(), display, goannaView);

                try {
                    presentation.show();
                } catch (WindowManager.InvalidDisplayException ex) {
                    Log.w(LOGTAG, "Couldn't show presentation!  Display was removed in "
                            + "the meantime.", ex);
                    presentation = null;
                }
            }
        } else if (presentation != null) {
            presentation.dismiss();
            presentation = null;
        }
    }

    @WrapForJNI(calledFrom = "ui")
    /* protected */ static native void invalidateAndScheduleComposite(GoannaView goannaView);

    @WrapForJNI(calledFrom = "ui")
    /* protected */ static native void addPresentationSurface(GoannaView goannaView, Surface surface);

    @WrapForJNI(calledFrom = "ui")
    /* protected */ static native void removePresentationSurface();

    private static final class GoannaPresentation extends Presentation {
        private SurfaceView mView;
        private GoannaView mGoannaView;

        public GoannaPresentation(Context context, Display display, GoannaView goannaView) {
            super(context, display);

            mGoannaView = goannaView;
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);

            mView = new SurfaceView(getContext());
            setContentView(mView, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            mView.getHolder().addCallback(new SurfaceListener(mGoannaView));
        }
    }

    private static final class SurfaceListener implements SurfaceHolder.Callback {
        private GoannaView mGoannaView;

        public SurfaceListener(GoannaView goannaView) {
            mGoannaView = goannaView;
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width,
                                   int height) {
            // Surface changed so force a composite
            if (GoannaThread.isStateAtLeast(GoannaThread.State.PROFILE_READY)) {
                invalidateAndScheduleComposite(mGoannaView);
            }
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            if (GoannaThread.isStateAtLeast(GoannaThread.State.PROFILE_READY)) {
                addPresentationSurface(mGoannaView, holder.getSurface());
            }
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            if (GoannaThread.isStateAtLeast(GoannaThread.State.PROFILE_READY)) {
                removePresentationSurface();
            }
        }
    }
}
