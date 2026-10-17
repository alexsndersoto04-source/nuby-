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
    private TextView hint;
    private TextView gameStatus;
    private TextView modeButton;
    private TextView restartButton;
    private TextView jumpButton;
    private AdventureJoystickView adventureJoystick;
    private volatile boolean gameMode = false;
    private volatile boolean adventureMode = false;
    private volatile int activeDemoMode = 0;
    private volatile boolean gameFinished = false;
    private float density;
    private long lastFrameNanos = 0L;
    private long fpsWindowStartNanos = 0L;
    private int framesInWindow = 0;
    private volatile String gpuName = "GPU";
    private volatile String shadowStatus = "SHADOWS CHECKING";
    private volatile String postStatus = "BLOOM CHECKING";
    private volatile String meshStatus = "MESH CHECKING";

    private static native void nativeCreate(android.content.res.AssetManager assets);
    private static native boolean nativeShadowsEnabled();
    private static native boolean nativePostProcessingEnabled();
    private static native boolean nativeSampleMeshLoaded();
    private static native boolean nativeSampleMaterialLoaded();
    private static native void nativeSetDemoMode(int mode);
    private static native void nativeAdventureMove(float horizontal, float forward);
    private static native void nativeAdventureJump();
    private static native void nativeRestartGame();
    private static native String nativeGameStatus();
    private static native void nativeResize(int width, int height);
    private static native void nativeFrame(float deltaSeconds);
    private static native void nativeTouch(
            int action, float x, float y, float pinchDistance, int pointerCount);

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
            int pointerCount = Math.min(event.getPointerCount(), 2);
            int firstIndex = 0;
            int secondIndex = 1;
            if (action == MotionEvent.ACTION_POINTER_UP) {
                final int liftedIndex = event.getActionIndex();
                firstIndex = -1;
                secondIndex = -1;
                for (int index = 0; index < event.getPointerCount(); ++index) {
                    if (index == liftedIndex) continue;
                    if (firstIndex < 0) firstIndex = index;
                    else if (secondIndex < 0) secondIndex = index;
                }
                pointerCount = Math.min(event.getPointerCount() - 1, 2);
            }

            float x = 0.0f;
            float y = 0.0f;
            float pinchDistance = 0.0f;
            if (pointerCount >= 2 && firstIndex >= 0 && secondIndex >= 0) {
                final float firstX = event.getX(firstIndex);
                final float firstY = event.getY(firstIndex);
                final float secondX = event.getX(secondIndex);
                final float secondY = event.getY(secondIndex);
                x = (firstX + secondX) * 0.5f;
                y = (firstY + secondY) * 0.5f;
                pinchDistance = (float) Math.hypot(secondX - firstX, secondY - firstY);
            } else if (firstIndex >= 0) {
                x = event.getX(firstIndex);
                y = event.getY(firstIndex);
            }

            final float touchX = x;
            final float touchY = y;
            final float span = pinchDistance;
            final int pointers = pointerCount;
            // Keep all renderer state on GLSurfaceView's GL thread.
            surface.queueEvent(() -> nativeTouch(action, touchX, touchY, span, pointers));
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

        gameStatus = label("AETHER RUN  0/6  45s", 11, 0xFFF1F8FF, true);
        gameStatus.setGravity(Gravity.CENTER);
        gameStatus.setPadding(dp(16), dp(9), dp(16), dp(9));
        gameStatus.setBackground(pill(0xD8141B2D, dp(18)));
        FrameLayout.LayoutParams gameStatusParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        gameStatusParams.topMargin = dp(88);
        root.addView(gameStatus, gameStatusParams);
        gameStatus.setVisibility(View.GONE);

        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.HORIZONTAL);
        controls.setGravity(Gravity.CENTER_VERTICAL);
        controls.setPadding(dp(18), dp(4), dp(18), dp(4));
        hint = label("DRAG TO ORBIT", 10, 0xFFD7E6F5, true);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(dp(15), dp(9), dp(15), dp(9));
        hint.setBackground(pill(0xB8141B2D, dp(18)));
        controls.addView(hint, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        restartButton = label("RESTART RUN", 10, 0xFFD7E6F5, true);
        restartButton.setGravity(Gravity.CENTER);
        restartButton.setPadding(dp(15), dp(10), dp(15), dp(10));
        restartButton.setBackground(pill(0xB8141B2D, dp(18)));
        restartButton.setVisibility(View.GONE);
        actions.addView(restartButton);

        modeButton = label("START ADVENTURE", 10, 0xFFF2F5FF, true);
        modeButton.setGravity(Gravity.CENTER);
        modeButton.setPadding(dp(16), dp(10), dp(16), dp(10));
        modeButton.setBackground(pill(0xD826746F, dp(18)));
        LinearLayout.LayoutParams modeParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        modeParams.leftMargin = dp(8);
        actions.addView(modeButton, modeParams);
        controls.addView(actions);

        FrameLayout.LayoutParams controlsParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM);
        controlsParams.bottomMargin = dp(10);
        root.addView(controls, controlsParams);

        adventureJoystick = new AdventureJoystickView(this, (horizontal, forward) ->
                surface.queueEvent(() -> nativeAdventureMove(horizontal, forward)));
        adventureJoystick.setVisibility(View.GONE);
        FrameLayout.LayoutParams joystickParams = new FrameLayout.LayoutParams(
                dp(148), dp(148), Gravity.BOTTOM | Gravity.START);
        joystickParams.setMargins(dp(18), 0, 0, dp(70));
        root.addView(adventureJoystick, joystickParams);

        jumpButton = label("JUMP", 11, 0xFFFFF4D5, true);
        jumpButton.setGravity(Gravity.CENTER);
        jumpButton.setPadding(dp(19), dp(16), dp(19), dp(16));
        jumpButton.setBackground(pill(0xD8A85E28, dp(30)));
        jumpButton.setContentDescription("Saltar");
        jumpButton.setVisibility(View.GONE);
        FrameLayout.LayoutParams jumpParams = new FrameLayout.LayoutParams(
                dp(88), dp(64), Gravity.BOTTOM | Gravity.END);
        jumpParams.setMargins(0, 0, dp(22), dp(112));
        root.addView(jumpButton, jumpParams);
        jumpButton.setOnClickListener(view ->
                surface.queueEvent(MainActivity::nativeAdventureJump));

        modeButton.setOnClickListener(view -> {
            activeDemoMode = activeDemoMode == 0 ? 2 : activeDemoMode == 2 ? 1 : 0;
            gameMode = activeDemoMode != 0;
            adventureMode = activeDemoMode == 2;
            gameFinished = false;
            final int selectedMode = activeDemoMode;
            surface.queueEvent(() -> nativeSetDemoMode(selectedMode));
            hint.setText(activeDemoMode == 0 ? "DRAG TO ORBIT"
                    : activeDemoMode == 1 ? "SWIPE LEFT / RIGHT TO MOVE"
                    : "LEFT: MOVE  ·  RIGHT: LOOK");
            modeButton.setText(activeDemoMode == 0 ? "START ADVENTURE"
                    : activeDemoMode == 1 ? "ENGINE LAB" : "AETHER RUN");
            gameStatus.setVisibility(gameMode ? View.VISIBLE : View.GONE);
            adventureJoystick.setVisibility(adventureMode ? View.VISIBLE : View.GONE);
            jumpButton.setVisibility(adventureMode ? View.VISIBLE : View.GONE);
            restartButton.setVisibility(View.GONE);
        });
        restartButton.setOnClickListener(view -> {
            gameFinished = false;
            restartButton.setVisibility(View.GONE);
            surface.queueEvent(MainActivity::nativeRestartGame);
        });

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
        nativeCreate(getAssets());
        if (activeDemoMode != 0) nativeSetDemoMode(activeDemoMode);
        String detected = gl.glGetString(GL10.GL_RENDERER);
        gpuName = detected == null ? "GPU desconocida" : detected.trim();
        if (gpuName.length() > 26) gpuName = gpuName.substring(0, 26);
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        nativeResize(width, height);
        shadowStatus = nativeShadowsEnabled() ? "SHADOWS ON" : "SHADOWS OFF";
        postStatus = nativePostProcessingEnabled() ? "BLOOM ON" : "BLOOM OFF";
        boolean sampleMeshLoaded = nativeSampleMeshLoaded();
        meshStatus = !sampleMeshLoaded ? "MESH FALLBACK"
                : nativeSampleMaterialLoaded() ? "OBJ+MTL ON" : "MESH ON";
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
            final String currentGameStatus = nativeGameStatus();
            runOnUiThread(() -> {
                if (telemetry != null) telemetry.setText(
                        String.format(Locale.US, "%d FPS  ·  %s  ·  %s  ·  %s  ·  %s",
                                fps, gpuName, shadowStatus, postStatus, meshStatus));
                if (gameMode && gameStatus != null) {
                    gameStatus.setText(currentGameStatus);
                    gameStatus.setVisibility(View.VISIBLE);
                    boolean finished = currentGameStatus.contains("RUN OVER")
                            || currentGameStatus.contains("COMPLET");
                    if (finished != gameFinished) {
                        gameFinished = finished;
                        restartButton.setText(adventureMode ? "RESTART ADVENTURE" : "RESTART RUN");
                        restartButton.setVisibility(finished ? View.VISIBLE : View.GONE);
                    }
                }
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
