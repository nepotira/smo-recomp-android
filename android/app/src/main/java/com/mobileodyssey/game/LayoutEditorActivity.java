/* LayoutEditorActivity - the touch controls' editor: drag a control where it suits the player, make it bigger or smaller.
 *
 * Landscape, in the same full-screen window as the game (GameActivity), so a place here is the same place
 * in the game. Positions are parts of the window's width and height and sizes in parts of its height, as the runtime
 * keeps them (runtime/android/nnpad_android.c, pos_<control> = x y size in config/controls.ini). Only controls the player
 * moved or resized get a line; Reset removes them all. The game reads the file again while it runs.
 * Android port (2026-11-03). */
package com.mobileodyssey.game;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

public class LayoutEditorActivity extends Activity {
    /* The layout as made: the same table as k_buttons and k_stick in nnpad_android.c (keep the two in step). */
    static final String[] KEYS = {"A", "B", "X", "Y", "L", "ZL", "R", "ZR", "MINUS", "PLUS", "R3", "SHAKE", "LEFT", "RIGHT", "UP", "DOWN", "EXIT", "LSTICK", "RSTICK"};
    static final String[] LABELS = {"A", "B", "X", "Y", "L", "ZL", "R", "ZR", "-", "+", "R3", "~", "<", ">", "^", "v", "\u2302", "", ""};
    static final float[][] DEF = {      /* x, y, radius in parts of the height */
        {0.930f, 0.700f, 0.070f}, {0.855f, 0.835f, 0.070f}, {0.855f, 0.565f, 0.070f}, {0.780f, 0.700f, 0.070f},
        {0.090f, 0.200f, 0.060f}, {0.090f, 0.070f, 0.055f}, {0.910f, 0.200f, 0.060f}, {0.910f, 0.070f, 0.055f},
        {0.420f, 0.060f, 0.045f}, {0.580f, 0.060f, 0.045f}, {0.690f, 0.880f, 0.050f}, {0.690f, 0.540f, 0.055f},
        {0.250f, 0.870f, 0.042f}, {0.330f, 0.870f, 0.042f}, {0.290f, 0.790f, 0.042f}, {0.290f, 0.950f, 0.042f},
        {0.250f, 0.060f, 0.040f},
        {0.140f, 0.700f, 0.130f}, {0.560f, 0.720f, 0.130f}};
    static final int N = KEYS.length, LSTICK = 17, RSTICK = 18;

    LauncherActivity.Ini ini;
    float[] x = new float[N], y = new float[N], size = new float[N];
    float scale = 1f, opacity = 0.45f;
    boolean cameraStick = true;
    int sel = -1;
    Board board;
    TextView info;

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) GameActivity.fullScreen(this);
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        GameActivity.fullScreen(this);                    /* the game's window, so the places match */
        ini = new LauncherActivity.Ini(new File(new File(getExternalFilesDir(null), "config"), "controls.ini"));
        scale = clamp(LauncherActivity.parse(ini.get("touch_scale", "1.0"), 1f), 0.6f, 1.6f);
        opacity = clamp(LauncherActivity.parse(ini.get("touch_opacity", "0.45"), 0.45f), 0.25f, 1f);
        cameraStick = !"drag".equalsIgnoreCase(ini.get("camera_touch", "stick"));
        load();

        FrameLayout page = new FrameLayout(this);
        page.setBackgroundColor(0xFF2B3340);
        board = new Board(this);
        page.addView(board, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout bar = new LinearLayout(this);            /* the tools, in the middle of the screen where no control sits */
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setGravity(Gravity.CENTER_HORIZONTAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE0101318);
        bg.setCornerRadius(dp(14));
        bar.setBackground(bg);
        bar.setPadding(dp(10), dp(8), dp(10), dp(8));
        info = new TextView(this);
        info.setTextColor(0xFFECEFF4);
        info.setTextSize(13);
        info.setGravity(Gravity.CENTER);
        bar.addView(info);
        LinearLayout row1 = new LinearLayout(this), row2 = new LinearLayout(this);
        row1.addView(tool("Smaller", v -> resize(-0.1f)));
        row1.addView(tool("Bigger", v -> resize(0.1f)));
        row1.addView(tool("Put back", v -> { if (sel >= 0) { x[sel] = DEF[sel][0]; y[sel] = DEF[sel][1]; size[sel] = 1f; changed(); } }));
        row2.addView(tool("Reset all", v -> { for (int i = 0; i < N; i++) { x[i] = DEF[i][0]; y[i] = DEF[i][1]; size[i] = 1f; } sel = -1; changed(); }));
        row2.addView(tool("Cancel", v -> finish()));
        row2.addView(tool("Save", v -> { save(); finish(); }));
        bar.addView(row1);
        bar.addView(row2);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER_HORIZONTAL | Gravity.TOP);
        lp.topMargin = dp(70);
        page.addView(bar, lp);
        setContentView(page);
        changed();
    }

    void load() {
        for (int i = 0; i < N; i++) {
            x[i] = DEF[i][0]; y[i] = DEF[i][1]; size[i] = 1f;
            String v = ini.get("pos_" + KEYS[i], null);
            if (v == null) continue;
            String[] p = v.trim().split("\\s+");
            if (p.length >= 2) { x[i] = clamp(LauncherActivity.parse(p[0], x[i]), 0, 1); y[i] = clamp(LauncherActivity.parse(p[1], y[i]), 0, 1); }
            if (p.length >= 3) size[i] = clamp(LauncherActivity.parse(p[2], 1f), 0.5f, 2.5f);
        }
    }

    void save() {
        for (int i = 0; i < N; i++) {
            boolean moved = Math.abs(x[i] - DEF[i][0]) > 0.001f || Math.abs(y[i] - DEF[i][1]) > 0.001f || Math.abs(size[i] - 1f) > 0.001f;
            if (moved) ini.set("pos_" + KEYS[i], String.format(Locale.ROOT, "%.3f %.3f %.2f", x[i], y[i], size[i]));
            else ini.remove("pos_" + KEYS[i]);
        }
    }

    void resize(float d) {
        if (sel < 0) return;
        size[sel] = clamp(Math.round((size[sel] + d) * 10f) / 10f, 0.5f, 2.5f);
        changed();
    }
    void changed() {
        info.setText(sel < 0 ? "Drag a control to move it. Tap one, then Smaller / Bigger."
                : name(sel) + ": size " + Math.round(size[sel] * 100) + "%");
        board.invalidate();
    }
    String name(int i) { return i == LSTICK ? "Left stick" : i == RSTICK ? "Camera stick" : i == 16 ? "Exit" : KEYS[i].charAt(0) + KEYS[i].substring(1).toLowerCase(Locale.ROOT); }

    Button tool(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(13);
        GradientDrawable g = new GradientDrawable();
        g.setColor("Save".equals(s) ? LauncherActivity.ACCENT : 0xFF242A35);
        g.setCornerRadius(dp(10));
        b.setBackground(g);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(92), dp(40));
        lp.setMargins(dp(3), dp(4), dp(3), 0);
        b.setLayoutParams(lp);
        return b;
    }
    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
    static float clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

    /* The board: the controls drawn as the game draws them (circles of the same size, the same labels), dragged by a finger. */
    class Board extends View {
        final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), label = new Paint(Paint.ANTI_ALIAS_FLAG);
        float grabDx, grabDy;
        int drag = -1;

        Board(Context c) {
            super(c);
            line.setStyle(Paint.Style.STROKE);
            label.setTextAlign(Paint.Align.CENTER);
            label.setTypeface(Typeface.DEFAULT_BOLD);
            label.setColor(Color.WHITE);
        }
        float r(int i) { return DEF[i][2] * scale * size[i] * getHeight(); }
        boolean shown(int i) { return i != RSTICK || cameraStick; }

        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            line.setStrokeWidth(1);
            line.setColor(0x22FFFFFF);
            for (int i = 1; i < 10; i++) { c.drawLine(w * i / 10, 0, w * i / 10, h, line); c.drawLine(0, h * i / 10, w, h * i / 10, line); }
            line.setColor(0x55FFFFFF);
            c.drawCircle(0.5f * w, 0.06f * h, 0.032f * scale * h, line);          /* the on/off ring: fixed */
            int alpha = Math.round(255 * Math.max(opacity, 0.5f));
            for (int i = 0; i < N; i++) {
                if (!shown(i)) continue;
                float cx = x[i] * w, cy = y[i] * h, rr = r(i);
                boolean on = i == sel;
                if (i >= LSTICK) {                            /* a stick at rest: its ring and knob */
                    line.setStrokeWidth(dp(2));
                    line.setColor(on ? 0xFFFFD54F : Color.argb(alpha, 255, 255, 255));
                    c.drawCircle(cx, cy, rr, line);
                    fill.setColor(on ? 0xCCFFD54F : Color.argb(alpha, 220, 225, 235));
                    c.drawCircle(cx, cy, rr * 0.46f, fill);
                    label.setTextSize(dp(11));
                    c.drawText(i == LSTICK ? "MOVE" : "CAMERA", cx, cy + rr + dp(14), label);
                    continue;
                }
                fill.setColor(on ? 0xCCFFD54F : Color.argb(alpha, 70, 78, 92));
                c.drawCircle(cx, cy, rr, fill);
                line.setStrokeWidth(dp(on ? 3 : 2));
                line.setColor(on ? 0xFFFFD54F : Color.argb(alpha, 255, 255, 255));
                c.drawCircle(cx, cy, rr, line);
                label.setTextSize(Math.max(dp(10), rr * 0.8f));
                c.drawText(LABELS[i], cx, cy - (label.descent() + label.ascent()) / 2, label);
            }
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            float w = getWidth(), h = getHeight();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    drag = -1;
                    float best = 1e9f;
                    for (int i = 0; i < N; i++) {                 /* the nearest control under the finger, a little generous */
                        if (!shown(i)) continue;
                        float dx = e.getX() - x[i] * w, dy = e.getY() - y[i] * h, d = (float) Math.sqrt(dx * dx + dy * dy);
                        if (d < Math.max(r(i), dp(22)) * 1.15f && d < best) { best = d; drag = i; }
                    }
                    sel = drag;
                    if (drag >= 0) { grabDx = x[drag] * w - e.getX(); grabDy = y[drag] * h - e.getY(); }
                    changed();
                    return true;
                }
                case MotionEvent.ACTION_MOVE:
                    if (drag >= 0) {
                        x[drag] = clamp((e.getX() + grabDx) / w, 0, 1);
                        y[drag] = clamp((e.getY() + grabDy) / h, 0, 1);
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL:
                    drag = -1;
                    return true;
            }
            return super.onTouchEvent(e);
        }
    }
}
