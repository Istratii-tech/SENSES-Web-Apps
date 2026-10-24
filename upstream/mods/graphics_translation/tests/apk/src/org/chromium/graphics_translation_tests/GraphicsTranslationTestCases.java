/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.chromium.graphics_translation_tests;

import android.app.Activity;
import android.os.Bundle;
import android.test.AndroidTestCase;
import android.test.ActivityInstrumentationTestCase2;
import android.test.InstrumentationTestRunner;
import android.view.Surface;
import android.view.SurfaceView;

public class GraphicsTranslationTestCases
    extends ActivityInstrumentationTestCase2<GraphicsTranslationTestActivity> {
  public GraphicsTranslationTestCases() {
    super(GraphicsTranslationTestActivity.class);
  }

  @Override
  protected void setUp() throws Exception {
    super.setUp();

    // Note: getActivity() ensures that the Activity is launched.
    Activity activity = getActivity();
    SurfaceView view = (SurfaceView) activity.findViewById(R.id.surfaceview);
    setSurface(view.getHolder().getSurface());
  }

  @Override
  protected void tearDown() throws Exception {
    setSurface(null);
    super.tearDown();
  }

  public void testMain() {
    InstrumentationTestRunner testRunner =
        (InstrumentationTestRunner) getInstrumentation();
    Bundle arguments = testRunner.getArguments();
    String gtestList =
        arguments.getCharSequence("atf-gtest-list", "").toString();
    String gtestFilter =
        arguments.getCharSequence("atf-gtest-filter", "").toString();
    assertEquals(0, runTests(gtestList, gtestFilter));
  }

  private static native int runTests(String gtestList, String gtestFilter);
  private static native void setSurface(Surface surface);

  static {
    System.loadLibrary("graphics_translation_tests");
  }
}
