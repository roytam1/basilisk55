/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

package org.mozilla.goanna;

import android.app.Application;

import org.robolectric.TestLifecycleApplication;

import java.lang.reflect.Method;

/**
 * GoannaApplication isn't test-lifecycle friendly: onCreate is called multiple times, which
 * re-registers Goanna event listeners, which fails.  This class is magically named so that
 * Robolectric uses it instead of the application defined in the Android manifest.  See
 * http://robolectric.blogspot.ca/2013/04/the-test-lifecycle-in-20.html.
 */
public class TestGoannaApplication extends Application implements TestLifecycleApplication {
  @Override public void beforeTest(Method method) {
  }

  @Override public void prepareTest(Object test) {
  }

  @Override public void afterTest(Method method) {
  }
}
