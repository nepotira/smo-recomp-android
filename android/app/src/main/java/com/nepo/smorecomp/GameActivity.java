package com.nepo.smorecomp;

import android.app.NativeActivity;
import android.os.Build;
import android.os.Bundle;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * GameActivity - High performance NativeActivity host for SMO Recomp ARM64.
 * Loads libmain.so which dynamically links and executes smogame.so.
 */
public class GameActivity extends NativeActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Keep display awake during continuous gameplay
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Prevent Android touch boost power surge (120Hz thermal throttling prevention)
        if (Build.VERSION.SDK_INT >= 35) {
            getWindow().setFrameRateBoostOnTouchEnabled(false);
        }

        applyEdgeToEdgeImmersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyEdgeToEdgeImmersive();
        }
    }

    private void applyEdgeToEdgeImmersive() {
        Window window = getWindow();

        // Render behind punch-hole camera cutout for true edge-to-edge 20:9 viewport
        WindowManager.LayoutParams params = window.getAttributes();
        params.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(params);
        window.setDecorFitsSystemWindows(false);

        // Hide navigation and status bars completely
        WindowInsetsController controller = window.getDecorView().getWindowInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
}
