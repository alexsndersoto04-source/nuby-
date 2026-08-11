package com.nuby.browser;

import android.app.Activity;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.view.MotionEvent;

/** Surface real de Android: GLES se ejecuta en el GPU del teléfono. */
public final class MainActivity extends Activity {
  static { System.loadLibrary("nuby_gpu"); }
  private NubySurface surface;
  @Override public void onCreate(Bundle state) { super.onCreate(state); surface = new NubySurface(); setContentView(surface); }
  @Override public void onPause() { super.onPause(); surface.onPause(); }
  @Override public void onResume() { super.onResume(); surface.onResume(); }

  private final class NubySurface extends GLSurfaceView {
    NubySurface() { super(MainActivity.this); setEGLContextClientVersion(3); setRenderer(new NativeRenderer()); setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY); }
    @Override public boolean onTouchEvent(MotionEvent event) { nativeTouch(event.getActionMasked(), event.getX(), event.getY()); return true; }
  }
  private static final class NativeRenderer implements GLSurfaceView.Renderer {
    public void onSurfaceCreated(javax.microedition.khronos.opengles.GL10 gl, javax.microedition.khronos.egl.EGLConfig cfg) { nativeCreate(); }
    public void onSurfaceChanged(javax.microedition.khronos.opengles.GL10 gl, int w, int h) { nativeResize(w,h); }
    public void onDrawFrame(javax.microedition.khronos.opengles.GL10 gl) { nativeFrame(); }
  }
  private static native void nativeCreate();
  private static native void nativeResize(int width, int height);
  private static native void nativeFrame();
  private static native void nativeTouch(int action, float x, float y);
}
