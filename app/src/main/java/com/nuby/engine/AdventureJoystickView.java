package com.nuby.engine;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Color;
import android.view.MotionEvent;
import android.view.View;

/** Lightweight, native Android virtual stick for the adventure demo. */
final class AdventureJoystickView extends View {
    interface Listener {
        void onMove(float horizontal, float forward);
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Listener listener;
    private final float density;
    private final float radius;
    private final float knobRadius;
    private float centerX;
    private float centerY;
    private float knobX;
    private float knobY;
    private int activePointerId = MotionEvent.INVALID_POINTER_ID;

    AdventureJoystickView(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        density = getResources().getDisplayMetrics().density;
        radius = 57.0f * density;
        knobRadius = 24.0f * density;
        setLayerType(View.LAYER_TYPE_HARDWARE, null);
        setContentDescription("Movimiento del personaje");
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        centerX = getWidth() * 0.5f;
        centerY = getHeight() * 0.5f;
        if (activePointerId == MotionEvent.INVALID_POINTER_ID) {
            knobX = centerX;
            knobY = centerY;
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x66313B4D);
        canvas.drawCircle(centerX, centerY, radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2.0f * density);
        paint.setColor(0xAAE5D3A0);
        canvas.drawCircle(centerX, centerY, radius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(activePointerId == MotionEvent.INVALID_POINTER_ID
                ? 0xBFD7C18E : 0xE8F1D59F);
        canvas.drawCircle(knobX, knobY, knobRadius, paint);
        paint.setColor(Color.WHITE);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(9.0f * density);
        paint.setFakeBoldText(true);
        canvas.drawText("MOVE", centerX, getHeight() - 2.0f * density, paint);
        paint.setFakeBoldText(false);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            activePointerId = event.getPointerId(event.getActionIndex());
            updateFrom(event, event.getActionIndex());
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE && activePointerId != MotionEvent.INVALID_POINTER_ID) {
            int index = event.findPointerIndex(activePointerId);
            if (index >= 0) updateFrom(event, index);
            return true;
        }
        if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
                || action == MotionEvent.ACTION_POINTER_UP)
                && event.getPointerId(event.getActionIndex()) == activePointerId) {
            activePointerId = MotionEvent.INVALID_POINTER_ID;
            knobX = centerX;
            knobY = centerY;
            listener.onMove(0.0f, 0.0f);
            invalidate();
            return true;
        }
        return activePointerId != MotionEvent.INVALID_POINTER_ID;
    }

    private void updateFrom(MotionEvent event, int pointerIndex) {
        centerX = getWidth() * 0.5f;
        centerY = getHeight() * 0.5f;
        float dx = event.getX(pointerIndex) - centerX;
        float dy = event.getY(pointerIndex) - centerY;
        float length = (float) Math.hypot(dx, dy);
        float usableRadius = radius - knobRadius * 0.35f;
        if (length > usableRadius && length > 0.0f) {
            dx *= usableRadius / length;
            dy *= usableRadius / length;
        }
        knobX = centerX + dx;
        knobY = centerY + dy;
        listener.onMove(clamp(dx / usableRadius), clamp(-dy / usableRadius));
        invalidate();
    }

    private static float clamp(float value) {
        return Math.max(-1.0f, Math.min(1.0f, value));
    }
}
