/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna.process;

import org.mozilla.goanna.annotation.JNITarget;
import org.mozilla.goanna.GoannaAppShell;
import org.mozilla.goanna.mozglue.GoannaLoader;
import org.mozilla.goanna.GoannaThread;
import org.mozilla.goanna.mozglue.SafeIntent;
import org.mozilla.goanna.util.ThreadUtils;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.util.Log;

public class GoannaServiceChildProcess extends Service {

    static private String LOGTAG = "GoannaServiceChildProcess";

    private boolean serviceStarted;

    static private void stop() {
        ThreadUtils.postToUiThread(new Runnable() {
            @Override
            public void run() {
                Process.killProcess(Process.myPid());;
            }
        });
    }

    public void onCreate() {
        super.onCreate();
    }

    public void onDestroy() {
        super.onDestroy();
    }

    public int onStartCommand(final Intent intent, final int flags, final int startId) {
        return Service.START_STICKY;
    }

    private Binder mBinder = new IChildProcess.Stub() {
        @Override
        public void stop() {
            GoannaServiceChildProcess.stop();
        }

        @Override
        public int getPid() {
            return Process.myPid();
        }

        @Override
        public void start(final String[] args,
                          final ParcelFileDescriptor crashReporterPfd,
                          final ParcelFileDescriptor ipcPfd) {
            if (serviceStarted) {
                Log.e(LOGTAG, "Attempting to start a service that has already been started.");
                return;
            }
            serviceStarted = true;
            final int crashReporterFd = crashReporterPfd != null ? crashReporterPfd.detachFd() : -1;
            final int ipcFd = ipcPfd != null ? ipcPfd.detachFd() : -1;
            ThreadUtils.postToUiThread(new Runnable() {
                @Override
                public void run() {
                    GoannaAppShell.ensureCrashHandling();
                    GoannaAppShell.setApplicationContext(getApplicationContext());
                    if (GoannaThread.initChildProcess(null, args, crashReporterFd, ipcFd, false)) {
                        GoannaThread.launch();
                    }
                }
            });
        }
    };

    public IBinder onBind(final Intent intent) {
        GoannaLoader.setLastIntent(new SafeIntent(intent));
        return mBinder;
    }

    public boolean onUnbind(Intent intent) {
        Log.i(LOGTAG, "Service has been unbound. Stopping.");
        stop();
        return false;
    }

    @JNITarget
    static public final class goannamediaplugin extends GoannaServiceChildProcess {}

    @JNITarget
    static public final class tab extends GoannaServiceChildProcess {}
}
