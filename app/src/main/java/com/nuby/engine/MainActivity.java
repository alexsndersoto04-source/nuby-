package com.nuby.engine;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** Android-native host for Nuby's custom C++ runtime and GLES renderer. */
public final class MainActivity extends Activity implements GLSurfaceView.Renderer {
    static {
        System.loadLibrary("nuby-engine");
    }

    private GLSurfaceView surface;
    private TextView telemetry;
    private float density;
    private long lastFrameNanos = 0L;
    private long fpsWindowStartNanos = 0L;
    private int framesInWindow = 0;
    private volatile String gpuName = "GPU";
    private volatile String shadowStatus = "SHADOWS CHECKING";

    private static native void nativeCreate();
    private static native boolean nativeShadowsEnabled();
    private static native void nativeResize(int width, int height);
    private static native void nativeFrame(float deltaSeconds);
    private static native void nativeTouch(int action, float x, float y);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        density = getResources().getDisplayMetrics().density;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyImmersiveMode();

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(8, 11, 23));

        surface = new GLSurfaceView(this);
        surface.setEGLContextClientVersion(3);
        surface.setPreserveEGLContextOnPause(false);
        surface.setRenderer(this);
        surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        surface.setOnTouchListener((view, event) -> {
            final int action = event.getActionMasked();
            final float x = event.getX();
            final float y = event.getY();
            // Keep all renderer state on GLSurfaceView's GL thread.
            surface.queueEvent(() -> nativeTouch(action, x, y));
            return true;
        });
        root.addView(surface, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        LinearLayout brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.VERTICAL);
        brand.setPadding(dp(16), dp(10), dp(16), dp(10));
        brand.setBackground(pill(0xB8141B2D, dp(14)));
        TextView title = label("NUBY  /  ENGINE LAB", 13, 0xFFF2F5FF, true);
        TextView subtitle = label("REAL-TIME 3D  ·  ANDROID NATIVE", 9, 0xFF8E9BB8, false);
        brand.addView(title);
        brand.addView(subtitle);
        FrameLayout.LayoutParams brandParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        brandParams.setMargins(dp(18), dp(18), 0, 0);
        root.addView(brand, brandParams);

        telemetry = label("CHECKING GPU  ·  GLES 3", 10, 0xFFB7E5DF, true);
        telemetry.setPadding(dp(14), dp(10), dp(14), dp(10));
        telemetry.setBackground(pill(0xB8141B2D, dp(18)));
        FrameLayout.LayoutParams telemetryParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        telemetryParams.setMargins(0, dp(18), dp(18), 0);
        root.addView(telemetry, telemetryParams);

        TextView hint = label("DRAG TO ORBIT", 10, 0xFFD7E6F5, true);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(dp(15), dp(9), dp(15), dp(9));
        hint.setBackground(pill(0xB8141B2D, dp(18)));
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        hintParams.bottomMargin = dp(18);
        root.addView(hint, hintParams);

        setContentView(root);
    }

    private TextView label(String text, float sizeSp, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(color);
        view.setTextSize(sizeSp);
        view.setLetterSpacing(bold ? 0.09f : 0.06f);
        view.setTypeface(Typeface.create("sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
        view.setIncludeFontPadding(false);
        return view;
    }

    private GradientDrawable pill(int color, int radius) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(radius);
        drawable.setStroke(dp(1), 0x334BD9D0);
        return drawable;
    }

    private int dp(float value) {
        return Math.round(value * density);
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        nativeCreate();
        shadowStatus = nativeShadowsEnabled() ? "SHADOWS ON" : "SHADOWS OFF";
        String detected = gl.glGetString(GL10.GL_RENDERER);
        gpuName = detected == null ? "GPU desconocida" : detected.trim();
        if (gpuName.length() > 26) gpuName = gpuName.substring(0, 26);
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        nativeResize(width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        long now = System.nanoTime();
        float deltaSeconds = lastFrameNanos == 0L
                ? 1.0f / 60.0f
                : Math.min((now - lastFrameNanos) / 1_000_000_000.0f, 0.05f);
        lastFrameNanos = now;
        nativeFrame(deltaSeconds);

        framesInWindow++;
        if (fpsWindowStartNanos == 0L) fpsWindowStartNanos = now;
        long elapsed = now - fpsWindowStartNanos;
        if (elapsed >= 1_000_000_000L) {
            final int fps = Math.round(framesInWindow * 1_000_000_000.0f / elapsed);
            framesInWindow = 0;
            fpsWindowStartNanos = now;
            runOnUiThread(() -> {
                if (telemetry != null) telemetry.setText(
                        String.format(Locale.US, "%d FPS  ·  %s  ·  %s", fps, gpuName, shadowStatus));
            });
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveMode();
        if (surface != null) surface.onResume();
    }

    @Override
    protected void onPause() {
        if (surface != null) surface.onPause();
        super.onPause();
    }

    private void applyImmersiveMode() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersiveMode();
    }
}
