/* GameActivity - the game's window: android.app.NativeActivity running libsmo.so (runtime/android/android_main.c),
 * with the system bars hidden. The theme alone hides only the status bar; phones with button navigation then keep
 * the navigation bar over the right edge in landscape, on top of the touch controls there (A, ZR, R). Immersive:
 * the bars come back for a moment with a swipe from the edge. The layout editor uses the same window (fullScreen()),
 * so a place there is the same place in the game.
 * Leaving the game: askExit (the touch Exit button, the controller's Exit key) asks first, with the game held; Exit
 * waits for any save in progress, then ends this process (":game" in the manifest) and the app's menu comes back. */
package com.mobileodyssey.game;

import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.app.NativeActivity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public class GameActivity extends NativeActivity {
    native void nativeHold(boolean on);                    /* android_main.c: the game stands still (as in the menus) */
    native void nativeQuiesce();                           /* android_main.c: waits for a save in progress, then no more file writes */
    native void nativeSaveCache(boolean leaving);          /* nvn_vk_m4.c: writes the pipeline cache if it has something new */
    native void nativeLoadState(long[] out);               /* android_main.c: frames shown, pipelines made ahead, of how many, all made */
    private Dialog exitDialog;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);      /* a first start, a cutscene, a controller: no touches for minutes */
        /* Heat (2026-11-03): every touch raised the display to 120 Hz for as long as fingers were on it (Samsung, Android
         * 16: the touch controls kept it there for whole sessions), whatever rate the game asks for (android_main.c). The
         * game shows 30 or 60 frames a second: the refreshes above that only cost power. Android 15+. */
        if (android.os.Build.VERSION.SDK_INT >= 35) getWindow().setFrameRateBoostOnTouchEnabled(false);
        fullScreen(this);
        showLoading();
    }

    /* The loading screen: a window of its own over the game's, drawn at once (the game's stays empty until its renderer
     * shows a frame: the opening animation stopped half way on it). Not focusable, not touchable: the game keeps the
     * focus and the input. Gone, faded, once the game has shown its own frames for a moment. */
    private Dialog loading;
    private final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());
    private long loadStart;
    private void showLoading() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER);
        col.setBackgroundColor(LauncherActivity.BG);
        android.widget.ImageView icon = new android.widget.ImageView(this);
        icon.setImageDrawable(getApplicationInfo().loadIcon(getPackageManager()));
        col.addView(icon, new LinearLayout.LayoutParams(px(96), px(96)));
        TextView title = new TextView(this);
        title.setText("Loading\u2026");
        title.setTextSize(20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(LauncherActivity.TEXT);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, px(14), 0, px(14));
        col.addView(title);
        android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setIndeterminate(true);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.ACCENT));
        bar.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.ACCENT));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.CARD2));
        col.addView(bar, new LinearLayout.LayoutParams(px(320), px(10)));
        TextView note = new TextView(this);
        note.setTextSize(13);
        note.setTextColor(LauncherActivity.DIM);
        note.setGravity(Gravity.CENTER);
        note.setPadding(px(24), px(14), px(24), 0);
        col.addView(note, new LinearLayout.LayoutParams(px(460), WindowManager.LayoutParams.WRAP_CONTENT));

        Dialog d = new Dialog(this, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        d.setContentView(col);
        d.setCancelable(false);
        Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(LauncherActivity.BG));
        w.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT);
        w.setWindowAnimations(0);
        w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS);
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        w.setAttributes(lp);
        w.setDecorFitsSystemWindows(false);
        loading = d;
        d.show();
        WindowInsetsController c = w.getDecorView().getWindowInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
        loadStart = android.os.SystemClock.uptimeMillis();
        long[] st = new long[4];
        Runnable poll = new Runnable() {
            long shownAt = -1;
            @Override public void run() {
                if (loading == null || isFinishing()) return;
                try { nativeLoadState(st); } catch (UnsatisfiedLinkError e) { ui.postDelayed(this, 100); return; }      /* the game's thread has not registered it yet */
                long now = android.os.SystemClock.uptimeMillis();
                if (st[0] > 0 && shownAt < 0) shownAt = now;
                if (shownAt >= 0 && now - shownAt > 600) { hideLoading(); return; }     /* the game's own frames for a moment */
                if (st[2] > 0 && st[3] == 0) {                 /* the pipelines made ahead: from the cache in seconds, from nothing on a first start */
                    bar.setIndeterminate(false);
                    bar.setMax((int)st[2]);
                    bar.setProgress((int)st[1]);
                    title.setText("Preparing graphics\u2026 " + (st[1] * 100 / st[2]) + "%");
                    if (now - loadStart > 8000) note.setText("The first start on this phone, or after an update, builds the graphics for it. "
                            + "This takes a minute or two once; the next starts take seconds.");
                } else if (st[3] != 0) {
                    bar.setIndeterminate(true);
                    title.setText("Starting the game\u2026");
                }
                ui.postDelayed(this, 100);
            }
        };
        ui.post(poll);
    }
    private void hideLoading() {
        Dialog d = loading;
        loading = null;
        if (d == null) return;
        View v = d.getWindow().getDecorView();
        v.animate().alpha(0f).setDuration(300).withEndAction(() -> { if (!isDestroyed()) d.dismiss(); }).start();
    }
    /* The pipeline cache is written here (the game's process can end at any time in the background, without an exit
     * path): the next start makes its pipelines from it instead of from nothing. */
    @Override protected void onStop() {
        super.onStop();
        if (!leaving) new Thread(() -> nativeSaveCache(false), "smo-pcache").start();
    }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) fullScreen(this);                       /* again after a dialog, the notification shade, another app */
    }

    /* Called by the game's input thread. A popup of the app's own look (LauncherActivity's colours) in the middle of the
     * screen; outside it or the back gesture keeps playing. */
    public void askExit() {
        runOnUiThread(() -> {
            if (exitDialog != null || isFinishing()) return;
            nativeHold(true);
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setPadding(px(24), px(22), px(24), px(20));
            card.setBackground(round(LauncherActivity.CARD, 22));
            TextView title = new TextView(this);
            title.setText("Leave the game?");
            title.setTextSize(21);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            title.setTextColor(LauncherActivity.TEXT);
            card.addView(title);
            TextView msg = new TextView(this);
            msg.setText("You go back to the app's menu. Progress since the game last saved is lost; a save in progress is "
                    + "finished first.");
            msg.setTextSize(14);
            msg.setTextColor(LauncherActivity.DIM);
            msg.setLineSpacing(0, 1.15f);
            msg.setPadding(0, px(8), 0, px(20));
            card.addView(msg);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            Button stay = button("Keep playing", LauncherActivity.CARD2);
            Button exit = button("Exit to the menu", LauncherActivity.ACCENT);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, px(50), 1);
            row.addView(stay, lp);
            lp = new LinearLayout.LayoutParams(0, px(50), 1);
            lp.setMargins(px(10), 0, 0, 0);
            row.addView(exit, lp);
            card.addView(row);

            Dialog d = new Dialog(this);
            d.requestWindowFeature(Window.FEATURE_NO_TITLE);
            d.setContentView(card);
            d.setCanceledOnTouchOutside(true);
            Window w = d.getWindow();
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            w.setLayout(Math.min(px(440), getResources().getDisplayMetrics().widthPixels - px(32)), WindowManager.LayoutParams.WRAP_CONTENT);
            w.setGravity(Gravity.CENTER);
            w.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            w.setDimAmount(0.6f);
            stay.setOnClickListener(v -> d.dismiss());
            exit.setOnClickListener(v -> { leaving = true; exit.setText("Leaving…"); exit.setEnabled(false); stay.setEnabled(false); leave(); });
            d.setOnDismissListener(x -> {
                if (leaving) return;
                exitDialog = null;
                nativeHold(false);
                fullScreen(this);
            });
            exitDialog = d;
            /* not focusable while it is shown, or the system bars come back with it; the bars of its own window hidden */
            w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            d.show();
            WindowInsetsController c = w.getDecorView().getWindowInsetsController();
            if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
            w.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        });
    }
    private boolean leaving;
    private void leave() {
        new Thread(() -> {
            nativeQuiesce();
            nativeSaveCache(true);
            runOnUiThread(() -> {
                if (isTaskRoot()) startActivity(new Intent(this, LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                finish();                                  /* the app's menu is under the game */
                /* android_main.c ends this process (the game's only, the app stays) once the activity is destroyed */
            });
        }, "smo-exit").start();
    }
    private int px(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
    private GradientDrawable round(int col, int r) { GradientDrawable g = new GradientDrawable(); g.setColor(col); g.setCornerRadius(px(r)); return g; }
    private Button button(String s, int col) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(15);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setStateListAnimator(null);
        b.setBackground(round(col, 14));
        return b;
    }

    /* The whole display: under the system bars and the camera cutout, the bars hidden until a swipe. */
    static void fullScreen(Activity a) {
        Window w = a.getWindow();
        WindowManager.LayoutParams p = w.getAttributes();
        p.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        w.setAttributes(p);
        w.setDecorFitsSystemWindows(false);
        WindowInsetsController c = w.getDecorView().getWindowInsetsController();    /* getDecorView makes the view first: Window.getInsetsController alone fails before setContentView (Samsung, Android 16) */
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}
