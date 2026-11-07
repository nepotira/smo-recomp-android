package com.nepo.smorecomp;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.RadioButton;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;

import java.io.File;

public class LauncherActivity extends AppCompatActivity {

    private static final String PREFS_NAME = "smo_recomp_prefs";
    private static final String KEY_ROMFS_PATH = "romfs_path";
    private static final String KEY_NATIVE_ASPECT = "native_aspect_20_9";
    private static final String KEY_TARGET_60FPS = "target_60fps";
    private static final String KEY_CUSTOM_TURNIP = "custom_turnip_driver";

    private SharedPreferences prefs;
    private TextView textRomFsStatus;
    private CheckBox checkNativeAspect;
    private CheckBox checkCustomTurnip;
    private RadioGroup radioFpsGroup;

    private final ActivityResultLauncher<Uri> openDocTreeLauncher =
        registerForActivityResult(new ActivityResultContracts.OpenDocumentTree(), uri -> {
            if (uri != null) {
                getContentResolver().takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                );
                String path = uri.toString();
                prefs.edit().putString(KEY_ROMFS_PATH, path).apply();
                updateRomFsDisplay(path);
            }
        });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Keep screen on and hide system bars
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars(this);

        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);

        buildUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars(this);
    }

    private void buildUi() {
        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(0xFF121212);
        scrollView.setFillViewport(true);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(32), dp(24), dp(32), dp(24));
        layout.setGravity(Gravity.CENTER_HORIZONTAL);

        // Header Title
        TextView title = new TextView(this);
        title.setText("Super Mario Odyssey — Recomp ARM64");
        title.setTextSize(24);
        title.setTextColor(0xFFFFFFFF);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, 0, 0, dp(8));
        layout.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("Dual-Library Native Architecture (libmain.so + smogame.so) · Silicon Tuned");
        subtitle.setTextSize(13);
        subtitle.setTextColor(0xFF888888);
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setPadding(0, 0, 0, dp(24));
        layout.addView(subtitle);

        // RomFS Selection Section
        Button btnPickRom = new Button(this);
        btnPickRom.setText("📁 Select RomFS Directory");
        btnPickRom.setBackgroundColor(0xFF2A2A2A);
        btnPickRom.setTextColor(0xFFFFFFFF);
        btnPickRom.setOnClickListener(v -> openDocTreeLauncher.launch(null));
        layout.addView(btnPickRom);

        textRomFsStatus = new TextView(this);
        textRomFsStatus.setTextSize(12);
        textRomFsStatus.setTextColor(0xFF00E676);
        textRomFsStatus.setGravity(Gravity.CENTER);
        textRomFsStatus.setPadding(0, dp(6), 0, dp(16));
        updateRomFsDisplay(prefs.getString(KEY_ROMFS_PATH, ""));
        layout.addView(textRomFsStatus);

        // Aspect Ratio Toggle
        checkNativeAspect = new CheckBox(this);
        checkNativeAspect.setText("Enable Hor+ 20:9 Native Screen Aspect Ratio (2.22295f — Zero Stretch)");
        checkNativeAspect.setTextColor(0xFFE0E0E0);
        checkNativeAspect.setChecked(prefs.getBoolean(KEY_NATIVE_ASPECT, true));
        checkNativeAspect.setOnCheckedChangeListener((btn, isChecked) ->
            prefs.edit().putBoolean(KEY_NATIVE_ASPECT, isChecked).apply());
        layout.addView(checkNativeAspect);

        // Custom Driver Toggle (Turnip 8xx / AdrenoTools)
        checkCustomTurnip = new CheckBox(this);
        checkCustomTurnip.setText("Enable Mesa Turnip 8xx UMA Zero-Copy Hook via AdrenoTools");
        checkCustomTurnip.setTextColor(0xFFE0E0E0);
        checkCustomTurnip.setChecked(prefs.getBoolean(KEY_CUSTOM_TURNIP, true));
        checkCustomTurnip.setOnCheckedChangeListener((btn, isChecked) ->
            prefs.edit().putBoolean(KEY_CUSTOM_TURNIP, isChecked).apply());
        layout.addView(checkCustomTurnip);

        // Target FPS selector
        TextView fpsLabel = new TextView(this);
        fpsLabel.setText("Frame Pacing Rate:");
        fpsLabel.setTextColor(0xFFB0B0B0);
        fpsLabel.setPadding(0, dp(12), 0, dp(4));
        layout.addView(fpsLabel);

        radioFpsGroup = new RadioGroup(this);
        radioFpsGroup.setOrientation(RadioGroup.HORIZONTAL);
        radioFpsGroup.setGravity(Gravity.CENTER);

        RadioButton rb60 = new RadioButton(this);
        rb60.setText("60 FPS (Native Performance)");
        rb60.setTextColor(0xFFE0E0E0);
        rb60.setId(View.generateViewId());

        RadioButton rb30 = new RadioButton(this);
        rb30.setText("30 FPS (Battery Conserve)");
        rb30.setTextColor(0xFFE0E0E0);
        rb30.setId(View.generateViewId());

        radioFpsGroup.addView(rb60);
        radioFpsGroup.addView(rb30);

        boolean is60 = prefs.getBoolean(KEY_TARGET_60FPS, true);
        if (is60) rb60.setChecked(true); else rb30.setChecked(true);

        radioFpsGroup.setOnCheckedChangeListener((group, checkedId) -> {
            boolean mode60 = (checkedId == rb60.getId());
            prefs.edit().putBoolean(KEY_TARGET_60FPS, mode60).apply();
        });
        layout.addView(radioFpsGroup);

        // Launch Button
        Button btnLaunch = new Button(this);
        btnLaunch.setText("▶ LAUNCH SUPER MARIO ODYSSEY");
        btnLaunch.setTextSize(16);
        btnLaunch.setBackgroundColor(0xFFE53935);
        btnLaunch.setTextColor(0xFFFFFFFF);
        btnLaunch.setPadding(dp(24), dp(16), dp(24), dp(16));
        LinearLayout.LayoutParams btnParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        btnParams.setMargins(0, dp(24), 0, dp(16));
        btnLaunch.setLayoutParams(btnParams);

        btnLaunch.setOnClickListener(v -> launchGame());
        layout.addView(btnLaunch);

        scrollView.addView(layout);
        setContentView(scrollView);
    }

    private void updateRomFsDisplay(String path) {
        if (path == null || path.isEmpty()) {
            textRomFsStatus.setText("Status: No RomFS path selected (Select game data folder)");
            textRomFsStatus.setTextColor(0xFFFF5252);
        } else {
            textRomFsStatus.setText("Status: RomFS directory linked");
            textRomFsStatus.setTextColor(0xFF00E676);
        }
    }

    private void launchGame() {
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("NATIVE_ASPECT_20_9", prefs.getBoolean(KEY_NATIVE_ASPECT, true));
        intent.putExtra("TARGET_60FPS", prefs.getBoolean(KEY_TARGET_60FPS, true));
        intent.putExtra("CUSTOM_TURNIP", prefs.getBoolean(KEY_CUSTOM_TURNIP, true));
        intent.putExtra("ROMFS_URI", prefs.getString(KEY_ROMFS_PATH, ""));
        startActivity(intent);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    public static void hideSystemBars(Activity activity) {
        WindowInsetsController controller = activity.getWindow().getDecorView().getWindowInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
}
