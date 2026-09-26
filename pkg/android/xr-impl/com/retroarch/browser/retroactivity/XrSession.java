package com.retroarch.browser.retroactivity;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Shows RetroArch on a CRT placed in the room, in passthrough. RetroArch renders into a
 * SurfaceTexture owned by the headset renderer instead of the activity window.
 */
public final class XrSession
{
  private static final String TAG = "RetroArch-XR";
  private static final String MODEL_FILE_NAME = "tv.glb";
  private static final String POSE_PREFS = "xr";
  private static final String KEY_TV_POSE = "tv_pose";
  private static final int VIDEO_HEIGHT = 1080;
  private static final long JOIN_TIMEOUT_MS = 3000;
  /* Action codes from xr_actions.h. */
  private static final int ACTION_POWER = 9;

  private static final String SETTINGS_PREFS = "xr_settings";

  private static XrSession current;

  private final Activity activity;
  private final AtomicBoolean frameAvailable = new AtomicBoolean(false);
  private Thread renderThread;
  private Thread attachThread;
  private File modelFile;

  /* Render thread only. */
  private XrPanel panel;
  private XrPanel.Row surroundings, dimming, saturation, padding, scanlines, scanlineFade,
      reflections, reach, customModel;

  /* Render thread only. */
  private SurfaceTexture surfaceTexture;
  private Surface surface;
  private boolean hasFrame;

  private XrSession(Activity activity)
  {
    this.activity = activity;
  }

  public static void onCreate(Activity activity)
  {
    if (!isQuest() || current != null)
      return;
    /* Before the activity window arrives, so RetroArch never binds to it. */
    nativeSetXrEnabled(true);
    current = new XrSession(activity);
    current.start();
  }

  public static void onDestroy(Activity activity)
  {
    XrSession session = current;
    if (session == null || session.activity != activity)
      return;
    XrNative.requestExit();
    join(session.renderThread);
    current = null;
  }

  private static boolean isQuest()
  {
    return "Oculus".equalsIgnoreCase(Build.MANUFACTURER)
        || "Meta".equalsIgnoreCase(Build.MANUFACTURER);
  }

  private static void join(Thread thread)
  {
    if (thread == null)
      return;
    try
    {
      thread.join(JOIN_TIMEOUT_MS);
    }
    catch (InterruptedException e)
    {
      Thread.currentThread().interrupt();
    }
  }

  private void start()
  {
    final float[] pose = loadPose();
    modelFile = new File(activity.getExternalFilesDir(null), MODEL_FILE_NAME);
    buildPanel();
    final String modelPath = modelPath();
    final float[] settings = settings();
    renderThread = new Thread(new Runnable()
    {
      @Override
      public void run()
      {
        if (!XrNative.run(activity, XrSession.this, pose, modelPath, settings))
        {
          Log.w(TAG, "OpenXR unavailable, using the normal window");
          activity.runOnUiThread(new Runnable()
          {
            @Override
            public void run()
            {
              nativeSetXrEnabled(false);
            }
          });
        }
      }
    }, "RetroArch-XR");
    renderThread.start();
  }

  /* ---- Called by the renderer, on its thread. ---- */

  void onGlReady(int texture, float screenAspect)
  {
    surfaceTexture = new SurfaceTexture(texture);
    surfaceTexture.setDefaultBufferSize(videoWidth(screenAspect), VIDEO_HEIGHT);
    surfaceTexture.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener()
    {
      @Override
      public void onFrameAvailable(SurfaceTexture st)
      {
        frameAvailable.set(true);
      }
    }, new Handler(Looper.getMainLooper()));
    surface = new Surface(surfaceTexture);

    /* Handing over the window waits for RetroArch's thread to take it, so keep that off
     * the render loop. */
    final Surface target = surface;
    attachThread = new Thread(new Runnable()
    {
      @Override
      public void run()
      {
        nativeSetXrSurface(target);
      }
    }, "RetroArch-XR-attach");
    attachThread.start();
  }

  boolean updateVideoTexture(float[] matrix)
  {
    if (surfaceTexture == null)
      return false;
    if (frameAvailable.getAndSet(false))
    {
      surfaceTexture.updateTexImage();
      hasFrame = true;
    }
    surfaceTexture.getTransformMatrix(matrix);
    return hasFrame;
  }

  boolean isPlaying()
  {
    return true;
  }

  void onAction(int code)
  {
    if (code == ACTION_POWER)
      nativeToggleMenu();
  }

  /* A different TV model changed the picture's shape; RetroArch follows the surface size. */
  void onScreenAspect(float screenAspect)
  {
    if (surfaceTexture != null)
      surfaceTexture.setDefaultBufferSize(videoWidth(screenAspect), VIDEO_HEIGHT);
  }

  boolean updatePanel(int texture)
  {
    return panel.upload(texture);
  }

  int onPanelTouch(float x, float y, boolean down)
  {
    return panel.touch(x, y, down);
  }

  /* The laser or a fingertip on the picture, 0..1 from its top left. */
  void onTouch(float x, float y, boolean down)
  {
    nativeSetXrTouch(x, y, down);
  }

  void onCrtPoseChanged(float[] pose)
  {
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < pose.length; i++)
    {
      if (i > 0)
        sb.append(',');
      sb.append(pose[i]);
    }
    prefs().edit().putString(KEY_TV_POSE, sb.toString()).apply();
  }

  void onSessionEnded()
  {
    join(attachThread);
    nativeSetXrTouch(0, 0, false);
    /* RetroArch must let go of the surface before the texture goes away. */
    if (surface != null)
      nativeSetXrSurface(null);
    if (surface != null)
      surface.release();
    if (surfaceTexture != null)
      surfaceTexture.release();
    surface = null;
    surfaceTexture = null;
    activity.runOnUiThread(new Runnable()
    {
      @Override
      public void run()
      {
        if (!activity.isFinishing())
          activity.finish();
      }
    });
  }

  private static int videoWidth(float screenAspect)
  {
    return Math.max(VIDEO_HEIGHT, Math.min(VIDEO_HEIGHT * 2,
        (int) (VIDEO_HEIGHT * screenAspect) / 2 * 2));
  }

  /* ---- Settings panel ---- */

  private void buildPanel()
  {
    SharedPreferences p = settingsPrefs();
    surroundings = XrPanel.choice("Surroundings", "Passthrough", "Dark room");
    surroundings.value = p.getInt("surroundings", 0);
    dimming = XrPanel.slider("Room dimming", 0, 95, 5, "%");
    dimming.value = p.getInt("room_dimming", 0);
    saturation = XrPanel.slider("Room colour", 0, 100, 5, "%");
    saturation.value = p.getInt("room_saturation", 100);
    padding = XrPanel.slider("Hand cutout padding", 0, 15, 1, " mm");
    padding.value = p.getInt("hand_padding_mm", 4);
    /* Off by default: RetroArch's own shaders handle the picture. */
    scanlines = XrPanel.slider("Scanlines", 0, 100, 5, "%");
    scanlines.value = p.getInt("scanlines", 0);
    scanlineFade = XrPanel.toggle("Fade scanlines with distance");
    scanlineFade.value = p.getInt("scanline_fade", 0);
    reflections = XrPanel.slider("Glass reflections", 0, 100, 5, "%");
    reflections.value = p.getInt("reflections", 100);
    reach = XrPanel.slider("Hand reach distance", 40, 100, 5, " cm");
    reach.value = p.getInt("reach_cm", 60);
    customModel = XrPanel.toggle("Custom TV model (" + MODEL_FILE_NAME + ")");
    customModel.value = p.getInt("custom_model", 1);
    panel = new XrPanel("Headset settings", new XrPanel.Listener()
    {
      @Override
      public void onValueChanged(XrPanel.Row row)
      {
        onSettingChanged(row);
      }
    }, surroundings, dimming, saturation, padding, scanlines, scanlineFade, reflections, reach,
        customModel,
        XrPanel.commands("TV size", XrPanel.CMD_SIZE_FIRST,
            "14\u2033", "20\u2033", "27\u2033", "32\u2033", "40\u2033"),
        XrPanel.commands("", XrPanel.CMD_RECENTER, "Recenter TV"));
    updateEnabled();
  }

  private void updateEnabled()
  {
    boolean passthrough = surroundings.value == 0;
    dimming.enabled = passthrough;
    saturation.enabled = passthrough;
    padding.enabled = passthrough;
    customModel.enabled = modelFile.isFile();
    panel.invalidate();
  }

  private void onSettingChanged(XrPanel.Row row)
  {
    settingsPrefs().edit()
        .putInt("surroundings", surroundings.value)
        .putInt("room_dimming", dimming.value)
        .putInt("room_saturation", saturation.value)
        .putInt("hand_padding_mm", padding.value)
        .putInt("scanlines", scanlines.value)
        .putInt("scanline_fade", scanlineFade.value)
        .putInt("reflections", reflections.value)
        .putInt("reach_cm", reach.value)
        .putInt("custom_model", customModel.value)
        .apply();
    updateEnabled();
    if (row == customModel)
      XrNative.setModel(modelPath());
    else
      XrNative.setSettings(settings());
  }

  private String modelPath()
  {
    return customModel.on() && modelFile.isFile() ? modelFile.getPath() : null;
  }

  /* Order matches XrSettings in xr_app.cpp. */
  private float[] settings()
  {
    return new float[] {
        padding.value / 1000f,
        1f - dimming.value / 100f,
        saturation.value / 100f,
        scanlines.value / 100f,
        scanlineFade.value,
        reflections.value / 100f,
        reach.value / 100f,
        surroundings.value,
    };
  }

  private SharedPreferences settingsPrefs()
  {
    return activity.getSharedPreferences(SETTINGS_PREFS, Activity.MODE_PRIVATE);
  }

  private SharedPreferences prefs()
  {
    return activity.getSharedPreferences(POSE_PREFS, Activity.MODE_PRIVATE);
  }

  private float[] loadPose()
  {
    String saved = prefs().getString(KEY_TV_POSE, null);
    if (saved == null)
      return null;
    String[] parts = saved.split(",");
    if (parts.length != 8)
      return null;
    float[] pose = new float[8];
    try
    {
      for (int i = 0; i < 8; i++)
        pose[i] = Float.parseFloat(parts[i]);
    }
    catch (NumberFormatException e)
    {
      return null;
    }
    return pose;
  }

  private static native void nativeSetXrEnabled(boolean enabled);

  private static native void nativeSetXrSurface(Surface surface);

  private static native void nativeToggleMenu();

  private static native void nativeSetXrTouch(float x, float y, boolean down);
}
