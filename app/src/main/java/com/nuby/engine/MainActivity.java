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
import android.widget.Toast;

import android.system.ErrnoException;
import android.system.Os;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
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
    private TextView editorButton;
    private TextView restartButton;
    private TextView jumpButton;
    private TextView editorSelection;
    private LinearLayout editorPanel;
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
    private static native int nativeRendererTier();
    private static native int nativeSceneAssetStatus();
    private static native String nativeAssetMemoryStatus();
    private static native String nativeLoadProject(String descriptorAssetPath);
    private static native boolean nativeUnloadProject();
    private static native String[] nativeSaveEditorProject();
    private static native String nativeLoadEditorProject(String descriptor, String scene);
    private static native boolean nativeEditorAddPrimitive(int type);
    private static native boolean nativeEditorSelectNext(int direction);
    private static native boolean nativeEditorTranslate(int axis, float amount);
    private static native boolean nativeEditorRotate(int axis, float amount);
    private static native boolean nativeEditorScale(float factor);
    private static native boolean nativeEditorCycleColor();
    private static native boolean nativeEditorDuplicate();
    private static native boolean nativeEditorDelete();
    private static native String nativeEditorSelectionStatus();
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

        editorButton = label("OPEN EDITOR", 10, 0xFFF2F5FF, true);
        editorButton.setGravity(Gravity.CENTER);
        editorButton.setPadding(dp(13), dp(10), dp(13), dp(10));
        editorButton.setBackground(pill(0xD85A518A, dp(18)));
        LinearLayout.LayoutParams editorButtonParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        editorButtonParams.leftMargin = dp(7);
        actions.addView(editorButton, editorButtonParams);
        controls.addView(actions);

        FrameLayout.LayoutParams controlsParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM);
        controlsParams.bottomMargin = dp(10);
        root.addView(controls, controlsParams);

        editorPanel = new LinearLayout(this);
        editorPanel.setOrientation(LinearLayout.VERTICAL);
        editorPanel.setPadding(dp(7), dp(6), dp(7), dp(6));
        editorPanel.setBackground(pill(0xD8141B2D, dp(12)));
        editorSelection = label("No object selected", 8, 0xFFB7E5DF, true);
        editorSelection.setPadding(dp(5), dp(2), dp(5), dp(5));
        editorPanel.addView(editorSelection);
        LinearLayout editorRowOne = new LinearLayout(this);
        editorRowOne.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout editorRowTwo = new LinearLayout(this);
        editorRowTwo.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout editorRowThree = new LinearLayout(this);
        editorRowThree.setOrientation(LinearLayout.HORIZONTAL);
        editorPanel.addView(editorRowOne);
        editorPanel.addView(editorRowTwo);
        editorPanel.addView(editorRowThree);
        addEditorButton(editorRowOne, "+ SPHERE", () -> queueEditorCommand(() -> nativeEditorAddPrimitive(0)));
        addEditorButton(editorRowOne, "+ TORUS", () -> queueEditorCommand(() -> nativeEditorAddPrimitive(1)));
        addEditorButton(editorRowOne, "PREV", () -> queueEditorCommand(() -> nativeEditorSelectNext(-1)));
        addEditorButton(editorRowOne, "NEXT", () -> queueEditorCommand(() -> nativeEditorSelectNext(1)));
        addEditorButton(editorRowOne, "DUP", () -> queueEditorCommand(MainActivity::nativeEditorDuplicate));
        addEditorButton(editorRowOne, "DELETE", () -> queueEditorCommand(MainActivity::nativeEditorDelete));
        addEditorButton(editorRowTwo, "COLOR", () -> queueEditorCommand(MainActivity::nativeEditorCycleColor));
        addEditorButton(editorRowTwo, "SAVE", this::saveEditorProject);
        addEditorButton(editorRowTwo, "LOAD", this::loadEditorProject);
        addEditorButton(editorRowTwo, "X−", () -> queueEditorCommand(() -> nativeEditorTranslate(0, -0.25f)));
        addEditorButton(editorRowTwo, "X+", () -> queueEditorCommand(() -> nativeEditorTranslate(0, 0.25f)));
        addEditorButton(editorRowTwo, "Y−", () -> queueEditorCommand(() -> nativeEditorTranslate(1, -0.25f)));
        addEditorButton(editorRowThree, "Y+", () -> queueEditorCommand(() -> nativeEditorTranslate(1, 0.25f)));
        addEditorButton(editorRowThree, "Z−", () -> queueEditorCommand(() -> nativeEditorTranslate(2, -0.25f)));
        addEditorButton(editorRowThree, "Z+", () -> queueEditorCommand(() -> nativeEditorTranslate(2, 0.25f)));
        addEditorButton(editorRowThree, "ROTATE", () -> queueEditorCommand(() -> nativeEditorRotate(1, 0.24f)));
        addEditorButton(editorRowThree, "SIZE−", () -> queueEditorCommand(() -> nativeEditorScale(0.85f)));
        addEditorButton(editorRowThree, "SIZE+", () -> queueEditorCommand(() -> nativeEditorScale(1.18f)));
        editorPanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams editorPanelParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM);
        editorPanelParams.setMargins(dp(10), 0, dp(10), dp(68));
        root.addView(editorPanel, editorPanelParams);

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
            int nextMode = activeDemoMode == 0 ? 2 : activeDemoMode == 2 ? 1 : 0;
            setActiveDemoMode(nextMode);
        });
        editorButton.setOnClickListener(view ->
                setActiveDemoMode(activeDemoMode == 3 ? 0 : 3));
        restartButton.setOnClickListener(view -> {
            gameFinished = false;
            restartButton.setVisibility(View.GONE);
            surface.queueEvent(MainActivity::nativeRestartGame);
        });

        setContentView(root);
    }

    private void setActiveDemoMode(int selectedMode) {
        activeDemoMode = selectedMode;
        gameMode = selectedMode == 1 || selectedMode == 2;
        adventureMode = selectedMode == 2;
        gameFinished = false;
        final String[] savedProject = selectedMode == 3 ? readSavedEditorProject() : null;
        surface.queueEvent(() -> {
            nativeSetDemoMode(selectedMode);
            if (selectedMode == 3 && savedProject != null) {
                nativeLoadEditorProject(savedProject[0], savedProject[1]);
            }
            final String selection = selectedMode == 3
                    ? nativeEditorSelectionStatus() : "";
            runOnUiThread(() -> {
                if (editorSelection != null && selectedMode == 3) {
                    editorSelection.setText(selection);
                }
            });
        });
        hint.setText(selectedMode == 0 ? "DRAG TO ORBIT"
                : selectedMode == 1 ? "SWIPE LEFT / RIGHT TO MOVE"
                : selectedMode == 2 ? "LEFT: MOVE  ·  RIGHT: LOOK"
                : "TAP OBJECT · TRANSFORM · SAVE PROJECT");
        modeButton.setText(selectedMode == 0 ? "START ADVENTURE"
                : selectedMode == 1 ? "ENGINE LAB" : "AETHER RUN");
        modeButton.setVisibility(selectedMode == 3 ? View.GONE : View.VISIBLE);
        editorButton.setText(selectedMode == 3 ? "EXIT EDITOR" : "OPEN EDITOR");
        gameStatus.setVisibility(gameMode ? View.VISIBLE : View.GONE);
        adventureJoystick.setVisibility(adventureMode ? View.VISIBLE : View.GONE);
        jumpButton.setVisibility(adventureMode ? View.VISIBLE : View.GONE);
        editorPanel.setVisibility(selectedMode == 3 ? View.VISIBLE : View.GONE);
        restartButton.setVisibility(View.GONE);
    }

    private void addEditorButton(LinearLayout row, String text, Runnable action) {
        TextView button = label(text, 8, 0xFFF2F5FF, true);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(4), dp(7), dp(4), dp(7));
        button.setMinWidth(dp(43));
        button.setBackground(pill(0xB826746F, dp(8)));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        params.setMargins(dp(2), dp(2), dp(2), dp(2));
        row.addView(button, params);
        button.setOnClickListener(view -> action.run());
    }

    private void queueEditorCommand(Runnable command) {
        if (surface == null) return;
        surface.queueEvent(() -> {
            command.run();
            final String selection = nativeEditorSelectionStatus();
            runOnUiThread(() -> {
                if (editorSelection != null) editorSelection.setText(selection);
            });
        });
    }

    private File editorProjectDirectory() {
        return new File(getFilesDir(), "nuby-projects/user-project");
    }

    private String[] readSavedEditorProject() {
        File root = editorProjectDirectory();
        File descriptor = new File(root, "project.nuby");
        File scene = new File(root, "Scenes/main.nuby");
        try {
            if (!descriptor.isFile() || !scene.isFile()) return null;
            return new String[]{readTextFile(descriptor, 64 * 1024),
                    readTextFile(scene, 16 * 1024 * 1024)};
        } catch (IOException exception) {
            return null;
        }
    }

    private String readTextFile(File file, int maximumBytes) throws IOException {
        if (file.length() <= 0L || file.length() > maximumBytes) {
            throw new IOException("Project file is empty or too large");
        }
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            int total = 0;
            while ((count = input.read(buffer)) != -1) {
                total += count;
                if (total > maximumBytes) throw new IOException("Project file is too large");
                output.write(buffer, 0, count);
            }
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private void writeTextFileAtomically(File target, String text) throws IOException {
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IOException("Could not create project folder");
        }
        File temporary = new File(parent, target.getName() + ".tmp");
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            output.write(bytes);
            output.getFD().sync();
        }
        try {
            Os.rename(temporary.getAbsolutePath(), target.getAbsolutePath());
        } catch (ErrnoException exception) {
            temporary.delete();
            throw new IOException("Could not replace saved project file", exception);
        }
    }

    private void saveEditorProject() {
        if (surface == null) return;
        surface.queueEvent(() -> {
            String[] project = nativeSaveEditorProject();
            runOnUiThread(() -> {
                if (project == null || project.length != 2) {
                    if (hint != null) hint.setText("NOTHING TO SAVE");
                    return;
                }
                try {
                    File root = editorProjectDirectory();
                    writeTextFileAtomically(new File(root, "Scenes/main.nuby"), project[1]);
                    writeTextFileAtomically(new File(root, "project.nuby"), project[0]);
                    if (hint != null) hint.setText("PROJECT SAVED ON THIS PHONE");
                    Toast.makeText(this, "Project saved", Toast.LENGTH_SHORT).show();
                } catch (IOException exception) {
                    if (hint != null) hint.setText("SAVE FAILED · " + exception.getMessage());
                }
            });
        });
    }

    private void loadEditorProject() {
        if (surface == null) return;
        final String[] project = readSavedEditorProject();
        if (project == null) {
            if (hint != null) hint.setText("NO SAVED PROJECT ON THIS PHONE");
            return;
        }
        surface.queueEvent(() -> {
            final String status = nativeLoadEditorProject(project[0], project[1]);
            final String selection = nativeEditorSelectionStatus();
            runOnUiThread(() -> {
                if (editorSelection != null) editorSelection.setText(selection);
                if (hint != null) hint.setText(status != null && status.startsWith("Project loaded")
                        ? "PROJECT REOPENED" : status);
            });
        });
    }

    /** Loads a standalone Nuby project packaged under Android assets. */
    public void loadProjectFromAssets(String descriptorAssetPath) {
        if (surface == null || descriptorAssetPath == null) return;
        surface.queueEvent(() -> {
            final String status = nativeLoadProject(descriptorAssetPath);
            runOnUiThread(() -> {
                if (status != null && status.startsWith("Loaded project")) {
                    resetDemoControls(status);
                } else if (hint != null) {
                    hint.setText(status);
                }
            });
        });
    }

    /** Unloads the active project and returns the renderer to its engine scene. */
    public void unloadProject() {
        if (surface == null) return;
        surface.queueEvent(() -> {
            final boolean unloaded = nativeUnloadProject();
            if (unloaded) runOnUiThread(() -> resetDemoControls("DRAG TO ORBIT"));
        });
    }

    private void resetDemoControls(String hintText) {
        activeDemoMode = 0;
        gameMode = false;
        adventureMode = false;
        gameFinished = false;
        if (hint != null) hint.setText(hintText);
        if (modeButton != null) {
            modeButton.setText("START ADVENTURE");
            modeButton.setVisibility(View.VISIBLE);
        }
        if (editorButton != null) {
            editorButton.setText("OPEN EDITOR");
            editorButton.setVisibility(View.VISIBLE);
        }
        if (gameStatus != null) gameStatus.setVisibility(View.GONE);
        if (restartButton != null) restartButton.setVisibility(View.GONE);
        if (adventureJoystick != null) adventureJoystick.setVisibility(View.GONE);
        if (jumpButton != null) jumpButton.setVisibility(View.GONE);
        if (editorPanel != null) editorPanel.setVisibility(View.GONE);
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
        if (activeDemoMode != 0) {
            nativeSetDemoMode(activeDemoMode);
            if (activeDemoMode == 3) {
                String[] savedProject = readSavedEditorProject();
                if (savedProject != null) nativeLoadEditorProject(savedProject[0], savedProject[1]);
            }
        }
        String detected = gl.glGetString(GL10.GL_RENDERER);
        gpuName = detected == null ? "GPU desconocida" : detected.trim();
        if (gpuName.length() > 26) gpuName = gpuName.substring(0, 26);
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        nativeResize(width, height);
        int rendererTier = nativeRendererTier();
        shadowStatus = rendererTier == 0 ? "BASIC RENDERER"
                : rendererTier == 1 ? "PBR NO CSM"
                : nativeShadowsEnabled() ? "SHADOWS ON" : "SHADOWS OFF";
        postStatus = nativePostProcessingEnabled() ? "BLOOM ON" : "BLOOM OFF";

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
            final String currentEditorSelection = activeDemoMode == 3
                    ? nativeEditorSelectionStatus() : "";
            final String currentMeshStatus = assetStatusLabel(nativeSceneAssetStatus());
            final String currentAssetMemoryStatus = nativeAssetMemoryStatus();
            runOnUiThread(() -> {
                meshStatus = currentMeshStatus;
                if (telemetry != null) telemetry.setText(
                        String.format(Locale.US,
                                "%d FPS  ·  %s  ·  %s  ·  %s  ·  %s  ·  %s",
                                fps, gpuName, shadowStatus, postStatus, meshStatus,
                                currentAssetMemoryStatus));
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
                if (activeDemoMode == 3 && editorSelection != null) {
                    editorSelection.setText(currentEditorSelection);
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

    private static String assetStatusLabel(int status) {
        switch (status) {
            case 1: return "MESH LOADING";
            case 2: return "TEXTURE LOADING";
            case 3: return "ASSETS READY";
            case 4: return "ASSET ERROR";
            default: return "ASSETS N/A";
        }
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
