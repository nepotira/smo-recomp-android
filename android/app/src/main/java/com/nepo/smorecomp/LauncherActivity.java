/* LauncherActivity - the app's front screen: the game, its settings, the driver and mods, then Play.
 *
 * Everything is built in code (no resources, no support libraries: tools/android/build_apk.py compiles this with javac
 * and d8 against android.jar). Settings are the runtime's own files in the app's external files folder:
 * config/graphics.ini (runtime/gfx_options.c) and config/controls.ini (runtime/android/nnpad_android.c); a value is
 * changed on its line and every comment stays. Play starts GameActivity (android.app.NativeActivity running libsmo.so).
 * Game files are the player's own: unpacked from their NSP / XCI with their prod.keys (GameImporter.java), or copied
 * from a folder they choose (the layout tools/android/push_data.py makes).
 * Android port (2026-11-02). */
package com.nepo.smorecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class LauncherActivity extends Activity {
    static final int BG = 0xFF101318, CARD = 0xFF1B2029, CARD2 = 0xFF242A35, TEXT = 0xFFECEFF4, DIM = 0xFF9AA3B2,
                     ACCENT = 0xFFE5393B, ACCENT2 = 0xFF3B82F6, OK = 0xFF34C77B, WARN = 0xFFF5A524;
    static final int REQ_GAME = 1, REQ_DRIVER = 2, REQ_MOD = 3, REQ_KEYS = 4, REQ_PACKAGE = 5, REQ_SAVE_OUT = 6, REQ_SAVE_IN = 7, REQ_LOGS = 8;
    java.util.Map<String, byte[]> keys;                  /* the parsed prod.keys while an NSP / XCI import runs (the file stays in keysFile()) */
    static final String[] TABS = {"Game", "Graphics", "Controls", "Advanced", "Mods"};

    File root, config;
    LinearLayout content;
    ScrollView scroll;                                   /* the page; its position is kept when the same page is drawn again */
    List<TextView> tabViews = new ArrayList<>();
    int tab = 0;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        root = getExternalFilesDir(null);
        config = new File(root, "config");
        config.mkdirs();
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(BG);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.VERTICAL);
        head.setPadding(dp(20), dp(18), dp(20), dp(6));
        head.addView(text("Odyssey", 26, TEXT, true));
        head.addView(text("Native Android port · beta " + appVersion() + " · your own game files", 13, DIM, false));
        page.addView(head);

        HorizontalScrollView tabScroll = new HorizontalScrollView(this);
        tabScroll.setHorizontalScrollBarEnabled(false);
        LinearLayout tabs = new LinearLayout(this);
        tabs.setPadding(dp(14), dp(6), dp(14), dp(6));
        for (int i = 0; i < TABS.length; i++) {
            final int t = i;
            TextView tv = text(TABS[i], 15, DIM, true);
            tv.setPadding(dp(16), dp(9), dp(16), dp(9));
            tv.setOnClickListener(v -> show(t));
            tabs.addView(tv);
            tabViews.add(tv);
        }
        tabScroll.addView(tabs);
        page.addView(tabScroll);

        ScrollView sv = new ScrollView(this);
        scroll = sv;
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(16), dp(8), dp(16), dp(24));
        sv.addView(content);
        page.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        page.setOnApplyWindowInsetsListener((v, in) -> {      /* Android 15 draws apps under the system bars: keep clear of them */
            v.setPadding(in.getSystemWindowInsetLeft(), in.getSystemWindowInsetTop(), in.getSystemWindowInsetRight(), in.getSystemWindowInsetBottom());
            return in;
        });
        setContentView(page);
        show(b != null ? b.getInt("tab", 0) : 0);         /* back from the landscape layout editor: the page the player was on */
    }

    @Override protected void onSaveInstanceState(Bundle out) { super.onSaveInstanceState(out); out.putInt("tab", tab); }
    @Override protected void onResume() { super.onResume(); if (content != null) show(tab); }

    void show(int t) {
        final int keep = t == tab && scroll != null ? scroll.getScrollY() : 0;      /* a setting changed: stay where the user was */
        tab = t;
        for (int i = 0; i < tabViews.size(); i++) {
            TextView tv = tabViews.get(i);
            tv.setTextColor(i == t ? TEXT : DIM);
            tv.setBackground(i == t ? round(CARD2, 18) : null);
        }
        content.removeAllViews();
        switch (t) {
            case 0: gamePage(); break;
            case 1: graphicsPage(); break;
            case 2: controlsPage(); break;
            case 3: advancedPage(); break;
            default: modsPage(); break;
        }
        if (keep > 0 && scroll != null) scroll.post(() -> scroll.scrollTo(0, keep));
    }

    /* ------------------------------------------------------------ pages */
    void gamePage() {
        boolean have = gameInstalled();
        LinearLayout card = card();
        card.addView(text("Super Mario Odyssey", 20, TEXT, true));
        card.addView(text("Version 1.0.0 · recompiled for this phone", 13, DIM, false));
        card.addView(space(12));
        card.addView(statusLine("Game files", have ? "installed" : "missing", have ? OK : WARN));
        card.addView(statusLine("Graphics driver", driverStatus(), DIM));
        Ini g = new Ini(new File(config, "graphics.ini"));
        card.addView(statusLine("Frame rate", "30".equals(g.get("frame_rate", "60")) ? "30 fps (full game speed)" : "60 fps", DIM));
        String rsv = g.get("resolution_scale", "1.00");
        card.addView(statusLine("Resolution", "auto".equalsIgnoreCase(rsv) ? ("handheld".equalsIgnoreCase(g.get("console_mode", "docked")) ? "1280x720 (console)" : "1920x1080 (console)") : Math.round(parse(rsv, 1f) * 100) + "% of the game's size", DIM));
        card.addView(space(16));
        Button play = button(have ? "▶  Play" : "Import game files first", have ? ACCENT : CARD2);
        play.setTextSize(18);
        play.setEnabled(have);
        play.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        card.addView(play, new LinearLayout.LayoutParams(-1, dp(56)));
        content.addView(card);

        LinearLayout imp = card();
        imp.addView(header("Game files"));
        imp.addView(text("From YOUR copy of the game: the prod.keys dumped from your own console, then an NSP or XCI of "
                + "version 1.0.0 (the base game, no update). The keys stay in this app's private storage (no other app can "
                + "read them) until you remove them; the game files are unpacked into this app's own folder. Nothing is sent "
                + "anywhere.", 13, DIM, false));
        imp.addView(space(8));
        boolean haveKeys = keysFile().exists();
        imp.addView(statusLine("1. prod.keys", haveKeys ? "imported" : "not imported", haveKeys ? OK : WARN));
        Button kb = button(haveKeys ? "Import prod.keys again…" : "Import prod.keys…", haveKeys ? CARD2 : ACCENT2);
        kb.setOnClickListener(v -> startActivityForResult(openAny(), REQ_KEYS));
        imp.addView(kb, new LinearLayout.LayoutParams(-1, dp(48)));
        if (haveKeys) {
            imp.addView(space(6));
            Button rk = button("Remove the keys", CARD2);
            rk.setOnClickListener(v -> confirm("Remove the prod.keys from this app?", "Remove", () -> { keysFile().delete(); toast("Keys removed"); show(0); }));
            imp.addView(rk, new LinearLayout.LayoutParams(-1, dp(48)));
        }
        imp.addView(space(10));
        imp.addView(statusLine("2. The game", have ? "installed" : "not installed", have ? OK : WARN));
        Button nsp = button("Import from NSP / XCI…", haveKeys ? ACCENT2 : CARD2);
        nsp.setOnClickListener(v -> {
            if (!keysFile().exists()) {
                new AlertDialog.Builder(this).setTitle("prod.keys first").setMessage("The game's files are encrypted: import the "
                        + "prod.keys dumped from your own console first (step 1).").setPositiveButton("OK", null).show();
                return;
            }
            startActivityForResult(openAny(), REQ_PACKAGE);
        });
        imp.addView(nsp, new LinearLayout.LayoutParams(-1, dp(48)));
        imp.addView(space(6));
        imp.addView(text("Or a folder the PC tools made (it holds romfs/ and dump/):", 12, DIM, false));
        imp.addView(space(4));
        Button pick = button("Import from a folder…", CARD2);
        pick.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_GAME));
        imp.addView(pick, new LinearLayout.LayoutParams(-1, dp(48)));
        imp.addView(space(6));
        imp.addView(text("Folder: " + root.getAbsolutePath(), 11, DIM, false));
        content.addView(imp);

        LinearLayout sv = card();
        sv.addView(header("Save data"));
        File[] files = listFiles(saveDir());
        long last = 0;
        for (File f : files) last = Math.max(last, f.lastModified());
        sv.addView(statusLine("Your save", files.length == 0 ? "none yet" : files.length + " files · "
                + new java.text.SimpleDateFormat("d MMM yyyy, HH:mm", java.util.Locale.getDefault()).format(new java.util.Date(last)), files.length == 0 ? DIM : OK));
        sv.addView(text("Export makes a .zip of your save to keep or move to another phone. Import replaces the save "
                + "with one from a .zip (from this app, or the save folder of the game from elsewhere); the current save is "
                + "kept as a backup in userdata/.", 12, DIM, false));
        sv.addView(space(8));
        Button ex = button("Export save…", ACCENT2);
        ex.setEnabled(files.length > 0);
        ex.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            i.putExtra(Intent.EXTRA_TITLE, "odyssey-save-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmm", java.util.Locale.ROOT).format(new java.util.Date()) + ".zip");
            startActivityForResult(i, REQ_SAVE_OUT);
        });
        sv.addView(ex, new LinearLayout.LayoutParams(-1, dp(48)));
        sv.addView(space(6));
        Button in = button("Import save…", CARD2);
        in.setOnClickListener(v -> {
            if (gameRunning()) { gameRunningDialog(); return; }
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            startActivityForResult(i, REQ_SAVE_IN);
        });
        sv.addView(in, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(sv);
    }

    void graphicsPage() {
        Ini g = new Ini(new File(config, "graphics.ini"));
        LinearLayout c = card();
        c.addView(header("Performance"));
        c.addView(choice(g, "frame_rate", "Frame rate", new String[]{"60", "30"}, new String[]{"60 fps", "30 fps, full speed"}, "60",
                "30 draws half the frames and runs the game's logic twice per frame, so it never plays in slow motion."));
        c.addView(slider(g, "fps_cap", "FPS limit", 20, 60, 5, 60, "", 1f,
                "60 = no limit. WARNING: the game advances one step per frame, so any limit below 60 makes it run in slow "
                + "motion (at 40, two thirds of its speed). Only \"30 fps, full speed\" above keeps normal speed; when it is "
                + "chosen, this limit is not used."));
        c.addView(choice(g, "boost", "CPU boost", new String[]{"auto", "on", "off"}, new String[]{"Auto", "Always", "Off"}, "on",
                "Keeps the phone's CPU clocks up so heavy scenes hold the frame rate. Auto: only while frames are late. Always: everywhere, warmer."));
        c.addView(choice(g, "shader_wait", "New shaders", new String[]{"hold", "skip"}, new String[]{"Wait", "Skip until ready"}, "hold",
                "Skip avoids a stutter the first time an effect appears; the effect is missing for a moment instead."));
        c.addView(choice(g, "diagnostics", "Diagnostics", new String[]{"off", "on"}, new String[]{"Off", "On"}, "off",
                "On: on Mali GPUs, about once a minute one frame is measured and small pictures of it go into the exported logs (a short hitch). Other GPUs: nothing."));
        content.addView(c);

        /* everything that decides the picture's size, together: the mode the game is told, then the scale of what it draws */
        boolean hh = "handheld".equalsIgnoreCase(g.get("console_mode", "docked"));
        int bw = hh ? 1280 : 1600, bh = hh ? 720 : 900;
        LinearLayout r = card();
        r.addView(header("Resolution"));
        r.addView(choice(g, "console_mode", "Console mode", new String[]{"docked", "handheld"}, new String[]{"Docked", "Handheld"}, "docked",
                "What the game is told it runs as; it picks its own picture size for each: docked " + "1600x900, handheld 1280x720."));
        r.addView(choice(g, "resolution_scale", "Resolution", new String[]{"auto", "1.50", "2.00"},
                new String[]{"Native\n" + (hh ? "1280x720" : "1920x1080"), "1.5x\n" + size(bw, bh, 1.5f), "2x\n" + size(bw, bh, 2f)}, "1.00",
                "Native: as the console shows the game (docked 1080p, handheld 720p). 1.5x and 2x multiply the game's own size, "
                + "as an emulator's resolution multiplier, and shrink the picture to your screen: much smoother edges, more "
                + "GPU work (2x held 30 fps in the 30 fps mode on a Galaxy S24 Ultra)."));
        String rv = g.get("resolution_scale", "1.00");
        float cur = "auto".equalsIgnoreCase(rv) ? (hh ? 1f : 1.2f) : parse(rv, 1f);
        r.addView(slider(g, "resolution_scale", "Custom scale", 25, 200, 5, Math.round(cur * 100), "%", 0.01f,
                "Now " + size(bw, bh, cur) + ". Your own scale of the game's " + bw + "x" + bh + "; moving it replaces the choice above."));
        content.addView(r);

        LinearLayout q = card();
        q.addView(header("Quality"));
        q.addView(choice(g, "anti_aliasing", "Anti-aliasing", new String[]{"off", "fxaa"}, new String[]{"Off", "FXAA"}, "fxaa",
                "FXAA smooths jagged edges on the final picture. Cheap; the picture gets a little softer."));
        q.addView(slider(g, "sharpening", "Sharpening", 0, 100, 5, 75, "%", 1f,
                "Contrast-adaptive: brings back fine detail (useful with FXAA or a high resolution scale) without hardening edges. 0 = off."));
        q.addView(choice(g, "anisotropic_filtering", "Anisotropic filtering", new String[]{"game", "2", "4", "8", "16"},
                new String[]{"Game", "2x", "4x", "8x", "16x"}, "game", "Sharper textures at an angle; costs a little GPU time."));
        content.addView(q);

        LinearLayout d = card();
        d.addView(header("Display"));
        d.addView(choice(g, "vsync", "Presentation", new String[]{"clock", "on", "mailbox", "off"},
                new String[]{"Clock", "VSync", "Mailbox", "Off"}, "mailbox", "Clock: lowest latency, paced at 60 Hz by the game itself."));
        d.addView(toggle(g, "fps_counter", "FPS counter", "on", "off", "on", "The number in the top left corner."));
        content.addView(d);
        content.addView(note("Changes apply the next time the game starts."));
    }

    void controlsPage() {
        Ini c = new Ini(new File(config, "controls.ini"));
        LinearLayout t = card();
        t.addView(header("Touch controls"));
        t.addView(choice(c, "touch", "Show touch controls", new String[]{"auto", "on", "off"}, new String[]{"Auto", "Always", "Off"}, "auto",
                "Auto hides them while a controller is in use. In game, the ring at the top switches them off and on."));
        t.addView(slider(c, "touch_opacity", "Opacity", 10, 100, 5, 45, "%", 0.01f, null));
        t.addView(slider(c, "touch_scale", "Size", 60, 160, 5, 100, "%", 0.01f, null));
        t.addView(choice(c, "camera_touch", "Camera", new String[]{"stick", "drag"}, new String[]{"Right stick", "Drag"}, "stick",
                "Right stick: a second stick that turns the camera. Drag: the camera follows your finger."));
        t.addView(slider(c, "camera_speed", "Camera speed", 20, 400, 10, 100, "%", 0.01f, null));
        t.addView(space(8));
        int placed = 0;
        for (String k : LayoutEditorActivity.KEYS) if (c.get("pos_" + k, null) != null) placed++;
        t.addView(statusLine("Layout", placed == 0 ? "as made" : placed + " controls placed by you", DIM));
        Button edit = button("Edit the layout…", ACCENT2);
        edit.setOnClickListener(v -> startActivity(new Intent(this, LayoutEditorActivity.class)));
        t.addView(edit, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(t);

        LinearLayout p = card();
        p.addView(header("Controller"));
        p.addView(choice(c, "layout", "Face buttons", new String[]{"positional", "labels"}, new String[]{"By position", "By label"}, "positional",
                "By position: the right button is A, as on the console. By label: the printed letters decide."));
        p.addView(slider(c, "deadzone", "Stick dead zone", 0, 50, 1, 15, "%", 0.01f, null));
        p.addView(toggle(c, "motion", "Motion controls", "on", "off", "on", "The phone's own sensors as the console's (Cappy throws)."));
        content.addView(p);

        LinearLayout k = card();
        k.addView(header("Controller buttons"));
        k.addView(text("Tap a button, then press the controller button you want for it. A key another button had moves "
                + "to that button, so nothing is left without a key. The sticks and analog triggers keep their own axes.", 12, DIM, false));
        boolean labels = "labels".equalsIgnoreCase(c.get("layout", "positional"));
        for (int i = 0; i < BIND_KEYS.length; i++) {
            final int b = i;
            int code = boundKey(c, b, labels);
            boolean own = c.get("bind_" + BIND_KEYS[b], null) != null;
            View row = statusLine(BIND_NAMES[b], keyName(code) + (own ? "  ✎" : ""), own ? ACCENT2 : TEXT);
            row.setPadding(0, dp(9), 0, dp(9));
            row.setOnClickListener(v -> captureKey(c, b, labels));
            k.addView(row);
        }
        k.addView(space(8));
        Button rb = button("Controller buttons to defaults", CARD2);
        rb.setOnClickListener(v -> confirm("Reset the controller buttons?", () -> { for (String s : BIND_KEYS) c.remove("bind_" + s); show(2); }));
        k.addView(rb, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(k);
        content.addView(note("Controller and touch changes are picked up while the game runs."));
    }

    /* ------------------------------------------------------------ controller bindings
     * bind_<button> = <Android key code> in controls.ini (nnpad_android.c key_bit): the key replaces the button's own. */
    static final String[] BIND_KEYS = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "LS", "R3", "PLUS", "MINUS", "UP", "DOWN", "LEFT", "RIGHT", "EXIT"};
    static final String[] BIND_NAMES = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Left stick click", "Right stick click", "+ (Plus)", "- (Minus)",
                                        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Exit the game (asks first)"};
    static int defaultKey(int b, boolean labels) {         /* key_bit_default in nnpad_android.c */
        switch (BIND_KEYS[b]) {
            case "A": return labels ? KeyEvent.KEYCODE_BUTTON_A : KeyEvent.KEYCODE_BUTTON_B;     /* by position: the right button */
            case "B": return labels ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A;
            case "X": return labels ? KeyEvent.KEYCODE_BUTTON_X : KeyEvent.KEYCODE_BUTTON_Y;
            case "Y": return labels ? KeyEvent.KEYCODE_BUTTON_Y : KeyEvent.KEYCODE_BUTTON_X;
            case "L": return KeyEvent.KEYCODE_BUTTON_L1;
            case "R": return KeyEvent.KEYCODE_BUTTON_R1;
            case "ZL": return KeyEvent.KEYCODE_BUTTON_L2;
            case "ZR": return KeyEvent.KEYCODE_BUTTON_R2;
            case "LS": return KeyEvent.KEYCODE_BUTTON_THUMBL;
            case "R3": return KeyEvent.KEYCODE_BUTTON_THUMBR;
            case "PLUS": return KeyEvent.KEYCODE_BUTTON_START;
            case "MINUS": return KeyEvent.KEYCODE_BUTTON_SELECT;
            case "UP": return KeyEvent.KEYCODE_DPAD_UP;
            case "DOWN": return KeyEvent.KEYCODE_DPAD_DOWN;
            case "LEFT": return KeyEvent.KEYCODE_DPAD_LEFT;
            case "EXIT": return KeyEvent.KEYCODE_BUTTON_MODE;      /* not a console button: nnpad_android.c g_exit_key */
            default: return KeyEvent.KEYCODE_DPAD_RIGHT;
        }
    }
    static int boundKey(Ini c, int b, boolean labels) {
        int k = (int) parse(c.get("bind_" + BIND_KEYS[b], "0"), 0);
        return k > 0 ? k : defaultKey(b, labels);
    }
    static String keyName(int code) {
        String s = KeyEvent.keyCodeToString(code);
        if (s.startsWith("KEYCODE_")) s = s.substring(8);
        switch (s) {
            case "BUTTON_THUMBL": return "Left stick click";
            case "BUTTON_THUMBR": return "Right stick click";
            case "BUTTON_L1": return "L1";
            case "BUTTON_R1": return "R1";
            case "BUTTON_L2": return "L2";
            case "BUTTON_R2": return "R2";
            case "BUTTON_MODE": return "Home / Guide";
        }
        if (s.startsWith("BUTTON_") && s.length() == 8) return "Button " + s.charAt(7);
        s = s.replace("BUTTON_", "").replace("DPAD_", "D-pad ").replace('_', ' ').toLowerCase(java.util.Locale.ROOT);
        return s.isEmpty() ? String.valueOf(code) : Character.toUpperCase(s.charAt(0)) + s.substring(1);
    }
    /* Waits for one key from a controller (not the phone's own keys) and gives it to button b. */
    void captureKey(Ini c, int b, boolean labels) {
        AlertDialog d = new AlertDialog.Builder(this).setTitle(BIND_NAMES[b])
                .setMessage("Press the button on your controller for " + BIND_NAMES[b] + ".\n\nNow: " + keyName(boundKey(c, b, labels)))
                .setNegativeButton("Cancel", null)
                .setNeutralButton("Default", (x, w) -> { c.remove("bind_" + BIND_KEYS[b]); show(2); }).create();
        d.setOnKeyListener((x, code, ev) -> {
            int src = ev.getSource();
            boolean pad = (src & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) != 0
                    || ((src & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD && ev.getDevice() != null && !ev.getDevice().isVirtual());
            if (!pad || code >= 512) return false;           /* the phone's back key and the like keep working */
            if (ev.getAction() != KeyEvent.ACTION_DOWN || ev.getRepeatCount() > 0) return true;
            int old = boundKey(c, b, labels);
            for (int o = 0; o < BIND_KEYS.length; o++)       /* the button that had this key gets b's old one */
                if (o != b && boundKey(c, o, labels) == code) setBind(c, o, old, labels);
            setBind(c, b, code, labels);
            d.dismiss();
            toast(BIND_NAMES[b] + " = " + keyName(code));
            show(2);
            return true;
        });
        d.show();
    }
    static void setBind(Ini c, int b, int code, boolean labels) {
        if (code == defaultKey(b, labels)) c.remove("bind_" + BIND_KEYS[b]);
        else c.set("bind_" + BIND_KEYS[b], String.valueOf(code));
    }

    void advancedPage() {
        Ini g = new Ini(new File(config, "graphics.ini"));
        LinearLayout c = card();
        c.addView(header("Graphics driver"));
        c.addView(text("Snapdragon phones can use a custom Vulkan driver (Mesa Turnip, an adrenotools package .zip). Other "
                + "phones (MediaTek, Exynos) always use their own driver.", 13, DIM, false));
        c.addView(space(8));
        c.addView(statusLine("Installed custom driver", customDriverName(), DIM));
        c.addView(choice(g, "driver", "Use", new String[]{"custom", "system"}, new String[]{"Custom if installed", "Phone's own"}, "custom", null));
        Button imp = button("Install a driver package (.zip)…", CARD2);
        imp.setOnClickListener(v -> { Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip"); startActivityForResult(i, REQ_DRIVER); });
        c.addView(imp, new LinearLayout.LayoutParams(-1, dp(48)));
        c.addView(space(6));
        Button rm = button("Remove the custom driver", CARD2);
        rm.setOnClickListener(v -> { deleteTree(new File(root, "driver")); toast("Custom driver removed"); show(3); });
        c.addView(rm, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(c);

        LinearLayout lg = card();
        lg.addView(header("Logs"));
        File log = new File(root, "log.txt");
        lg.addView(statusLine("Last game log", log.exists() ? new java.text.SimpleDateFormat("d MMM yyyy, HH:mm", java.util.Locale.getDefault())
                .format(new java.util.Date(log.lastModified())) : "none yet", DIM));
        lg.addView(text("Export makes a .zip to share when reporting a problem: the game's log of the last two runs, this "
                + "app's Android log (crashes), the settings files, and the phone model, Android version and app version. "
                + "No save, keys or game files.", 12, DIM, false));
        lg.addView(space(8));
        Button el = button("Export logs…", CARD2);
        el.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            i.putExtra(Intent.EXTRA_TITLE, "odyssey-logs-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmm", java.util.Locale.ROOT).format(new java.util.Date()) + ".zip");
            startActivityForResult(i, REQ_LOGS);
        });
        lg.addView(el, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(lg);

        LinearLayout r = card();
        r.addView(header("Reset"));
        Button rg = button("Graphics settings to defaults", CARD2);
        rg.setOnClickListener(v -> confirm("Reset graphics settings?", () -> { new File(config, "graphics.ini").delete(); show(3); }));
        r.addView(rg, new LinearLayout.LayoutParams(-1, dp(48)));
        r.addView(space(6));
        Button rc = button("Control settings to defaults", CARD2);
        rc.setOnClickListener(v -> confirm("Reset control settings?", () -> { new File(config, "controls.ini").delete(); show(3); }));
        r.addView(rc, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(r);
    }

    void modsPage() {
        File mods = new File(root, "mods");
        mods.mkdirs();
        Ini m = new Ini(new File(config, "mods.ini"));
        LinearLayout c = card();
        c.addView(header("Mods"));
        c.addView(text("A mod is a folder of files that replace the game's own (romfs/...). Mods higher in the list win "
                + "when two change the same file.", 13, DIM, false));
        c.addView(space(8));
        File[] list = mods.listFiles(File::isDirectory);
        if (list != null) java.util.Arrays.sort(list);          /* the order the game looks at them in (nnfs.c mods_load) */
        if (list == null || list.length == 0) c.addView(text("No mods installed.", 14, DIM, false));
        else for (File f : list) c.addView(toggle(m, f.getName(), f.getName(), "on", "off", "on", null));
        c.addView(space(10));
        Button imp = button("Install a mod (.zip)…", CARD2);
        imp.setOnClickListener(v -> { Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip"); startActivityForResult(i, REQ_MOD); });
        c.addView(imp, new LinearLayout.LayoutParams(-1, dp(48)));
        content.addView(c);
        content.addView(note("Mods: " + mods.getAbsolutePath()));
    }

    /* ------------------------------------------------------------ importing */
    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (req == REQ_KEYS) {
            byte[] raw;
            java.util.Map<String, byte[]> k;
            try { raw = readAll(getContentResolver().openInputStream(u)); k = GameImporter.parseKeys(new String(raw, StandardCharsets.UTF_8)); }
            catch (Exception e) { toast("Could not read that file: " + e.getMessage()); return; }
            boolean ok = k.containsKey("header_key");
            k.clear();
            if (!ok) { toast("That is not a prod.keys file (no header_key in it)"); return; }
            try (OutputStream o = new FileOutputStream(keysFile())) { o.write(raw); }
            catch (Exception e) { toast("Could not keep the keys: " + e.getMessage()); return; }
            java.util.Arrays.fill(raw, (byte) 0);
            toast("Keys imported");
            show(0);
        } else if (req == REQ_PACKAGE) {
            try { keys = GameImporter.parseKeys(new String(readAll(new FileInputStream(keysFile())), StandardCharsets.UTF_8)); }
            catch (Exception e) { toast("Could not read the imported keys: " + e.getMessage()); return; }
            importPackage(u);
        }
        else if (req == REQ_GAME) importGame(u);
        else if (req == REQ_DRIVER) background("Installing the driver…", () -> unzip(u, new File(root, "driver"), true), () -> show(3));
        else if (req == REQ_MOD) background("Installing the mod…", () -> unzip(u, new File(new File(root, "mods"), modName(u)), true), () -> show(4));
        else if (req == REQ_SAVE_OUT) background("Exporting the save…", () -> exportSave(u), () -> toast("Save exported"));
        else if (req == REQ_LOGS) background("Exporting the logs…", () -> exportLogs(u), () -> toast("Logs exported"));
        else if (req == REQ_SAVE_IN) confirmImportSave(u);
    }

    /* ------------------------------------------------------------ save data
     * <files>/userdata/save is the game's committed save (runtime/nnfs.c: the game's writes go to save.work.<pid> and
     * only Commit moves them there; save.commit is a commit to finish at the next start). */
    File userdata() { return new File(root, "userdata"); }
    File saveDir() { return new File(userdata(), "save"); }
    static File[] listFiles(File dir) {                 /* every file below dir */
        List<File> out = new ArrayList<>();
        java.util.ArrayDeque<File> todo = new java.util.ArrayDeque<>();
        todo.add(dir);
        while (!todo.isEmpty()) {
            File[] k = todo.poll().listFiles();
            if (k != null) for (File f : k) if (f.isDirectory()) todo.add(f); else out.add(f);
        }
        return out.toArray(new File[0]);
    }
    /* The game runs in this app's process: libsmo.so is loaded once it has started, until the app is closed. */
    /* The game runs in its own process (":game", GameActivity in the manifest), so it can end while this app stays. */
    int gamePid() {
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> l = getSystemService(android.app.ActivityManager.class).getRunningAppProcesses();
        if (l != null) for (android.app.ActivityManager.RunningAppProcessInfo p : l) if (p.processName.endsWith(":game")) return p.pid;
        return 0;
    }
    boolean gameRunning() { return gamePid() != 0; }
    void gameRunningDialog() {
        new AlertDialog.Builder(this).setTitle("The game is still running")
                .setMessage("A save can only be replaced while the game is closed, or the game would write over it. Leave the "
                        + "game with its Exit button (the house at the top of the touch controls), or close it here. Closing "
                        + "it here is like switching the console off: progress since the game last saved is lost.")
                .setPositiveButton("Close the game", (d, w) -> { int pid = gamePid(); if (pid != 0) android.os.Process.killProcess(pid); toast("Game closed"); })
                .setNegativeButton("Cancel", null).show();
    }
    void exportSave(Uri out) throws Exception {
        File dir = saveDir();
        String base = dir.getCanonicalPath() + File.separator;
        try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(getContentResolver().openOutputStream(out))) {
            for (File f : listFiles(dir)) {
                z.putNextEntry(new ZipEntry("save/" + f.getCanonicalPath().substring(base.length()).replace(File.separatorChar, '/')));
                try (InputStream in = new FileInputStream(f)) { pipe(in, z); }
                z.closeEntry();
            }
        }
    }
    /* For problem reports: the runtime's logs, this app's own logcat (Android shows an app only its own lines), the
     * settings and what phone it is. Nothing of the save, the keys or the game files. */
    void exportLogs(Uri out) throws Exception {
        try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(getContentResolver().openOutputStream(out))) {
            StringBuilder info = new StringBuilder();
            info.append("app ").append(appVersion()).append('\n')
                .append("android ").append(android.os.Build.VERSION.RELEASE).append(" (api ").append(android.os.Build.VERSION.SDK_INT).append(")\n")
                .append("phone ").append(android.os.Build.MANUFACTURER).append(' ').append(android.os.Build.MODEL).append('\n')
                .append("chip ").append(android.os.Build.SOC_MANUFACTURER).append(' ').append(android.os.Build.SOC_MODEL).append('\n')
                .append("abi ").append(String.join(" ", android.os.Build.SUPPORTED_ABIS)).append('\n')
                .append("custom driver ").append(customDriverName()).append('\n')
                .append("game files ").append(gameInstalled() ? "installed" : "missing").append('\n');
            z.putNextEntry(new ZipEntry("info.txt"));
            z.write(info.toString().getBytes(StandardCharsets.UTF_8));
            z.closeEntry();
            String[][] files = {{"log.txt", "log.txt"}, {"log.prev.txt", "log.prev.txt"}, {"config/graphics.ini", "graphics.ini"},
                                {"config/controls.ini", "controls.ini"}, {"config/mods.ini", "mods.ini"}, {"config/env.txt", "env.txt"}};
            for (String[] f : files) {
                File src = new File(root, f[0]);
                if (!src.isFile()) continue;
                z.putNextEntry(new ZipEntry(f[1]));
                try (InputStream in = new FileInputStream(src)) { pipe(in, z); }
                z.closeEntry();
            }
            File[] diag = new File(root, "diag").listFiles();  /* 0.1.3: the frames measured on this GPU (nvn_vk_draw.c, diag_present) */
            if (diag != null) for (File f : diag) {
                if (!f.isFile()) continue;
                z.putNextEntry(new ZipEntry("diag/" + f.getName()));
                try (InputStream in = new FileInputStream(f)) { pipe(in, z); }
                z.closeEntry();
            }
            Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-b", "main,system,crash").redirectErrorStream(true).start();
            z.putNextEntry(new ZipEntry("logcat.txt"));
            try (InputStream in = p.getInputStream()) { pipe(in, z); }
            z.closeEntry();
            p.waitFor();
        }
    }
    void confirmImportSave(Uri zip) {
        if (gameRunning()) { gameRunningDialog(); return; }
        new AlertDialog.Builder(this).setTitle("Replace your save?")
                .setMessage("The save in this .zip replaces the current one. The current save is kept as a backup folder in userdata/.")
                .setPositiveButton("Replace", (d, w) -> background("Importing the save…", () -> importSave(zip), () -> { toast("Save imported"); show(0); }))
                .setNegativeButton("Cancel", null).show();
    }
    void importSave(Uri zip) throws Exception {
        File tmp = new File(userdata(), "save.import");
        unzip(zip, tmp, true);
        File top = tmp;                                    /* the files may sit in a folder of the zip (save/ of an export) */
        for (File[] k; (k = top.listFiles()) != null && k.length == 1 && k[0].isDirectory(); ) top = k[0];
        if (listFiles(top).length == 0) { deleteTree(tmp); throw new java.io.IOException("there are no files in that .zip"); }
        File save = saveDir(), bak = null;
        if (save.exists()) {
            bak = new File(userdata(), "save-backup-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.ROOT).format(new java.util.Date()));
            if (!save.renameTo(bak)) { deleteTree(tmp); throw new java.io.IOException("could not keep the current save as a backup"); }
        }
        File[] old = userdata().listFiles();               /* a commit or working folder of the old save must not land on the new one */
        if (old != null) for (File f : old) {
            String n = f.getName();
            if (n.equals("save.commit") || n.startsWith("save.journal.") || n.startsWith("save.work.")) deleteTree(f);
        }
        if (!top.renameTo(save)) {
            if (bak != null) bak.renameTo(save);          /* the old save back where it was */
            deleteTree(tmp);
            throw new java.io.IOException("could not put the new save in place; your save is unchanged");
        }
        deleteTree(tmp);
    }

    Intent openAny() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        return i;
    }

    /* The player's NSP / XCI with their prod.keys (GameImporter); the parsed keys are dropped from memory when it ends. */
    void importPackage(Uri pkg) {
        final java.util.Map<String, byte[]> k = keys;
        keys = null;
        AlertDialog d = new AlertDialog.Builder(this).setTitle("Importing the game").setMessage("Starting…").setCancelable(false).show();
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            String err = null;
            try (android.os.ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(pkg, "r");
                 FileInputStream in = new FileInputStream(fd.getFileDescriptor())) {
                new GameImporter(in.getChannel(), k, s -> runOnUiThread(() -> d.setMessage(s))).run(root);
            } catch (Exception e) { err = e.getMessage(); }
            k.clear();
            final String fe = err;
            runOnUiThread(() -> {
                getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                d.dismiss();
                if (fe != null) new AlertDialog.Builder(this).setTitle("Import failed").setMessage(fe).setPositiveButton("OK", null).show();
                else toast("Game imported");
                show(0);
            });
        }).start();
    }

    void importGame(Uri tree) {
        background("Copying game files…", () -> {
            String doc = DocumentsContract.getTreeDocumentId(tree);
            copyTree(tree, doc, root, 0);
        }, () -> { toast(gameInstalled() ? "Game files imported" : "romfs/ or dump/ not found in that folder"); show(0); });
    }

    void copyTree(Uri tree, String docId, File dest, int depth) throws Exception {
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (android.database.Cursor cur = getContentResolver().query(kids, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (cur != null && cur.moveToNext()) {
                String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                if (depth == 0 && !(name.equals("romfs") || name.equals("dump") || name.equals("sysdata") || name.equals("manual"))) continue;
                File out = new File(dest, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) { out.mkdirs(); copyTree(tree, id, out, depth + 1); }
                else try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                          OutputStream os = new FileOutputStream(out)) { pipe(in, os); }
            }
        }
    }

    void unzip(Uri zip, File dest, boolean fresh) throws Exception {
        if (fresh) deleteTree(dest);
        dest.mkdirs();
        String base = dest.getCanonicalPath() + File.separator;
        try (ZipInputStream z = new ZipInputStream(getContentResolver().openInputStream(zip))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                File out = new File(dest, e.getName());
                if (!out.getCanonicalPath().startsWith(base)) continue;          /* no entry may write outside the folder */
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream os = new FileOutputStream(out)) { pipe(z, os); }
            }
        }
    }

    String modName(Uri u) {
        String n = u.getLastPathSegment();
        n = n == null ? "mod" : n.substring(n.lastIndexOf('/') + 1).replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9 _.-]", "_");
        return n.isEmpty() ? "mod" : n;
    }

    /* ------------------------------------------------------------ state */
    File keysFile() { return new File(getFilesDir(), "prod.keys"); }       /* internal storage: this app only */
    boolean gameInstalled() {
        File romfs = new File(root, "romfs"), npdm = new File(root, "dump/base/exefs/main.npdm");
        String[] k = romfs.list();
        return k != null && k.length > 0 && npdm.exists();
    }
    String customDriverName() {
        File d = new File(root, "driver"), meta = new File(d, "meta.json");
        if (!d.isDirectory()) return "none";
        try {
            String s = new String(readAll(new FileInputStream(meta)), StandardCharsets.UTF_8);
            int i = s.indexOf("\"name\"");
            if (i >= 0) { int a = s.indexOf('"', s.indexOf(':', i) + 1), b2 = s.indexOf('"', a + 1); if (a > 0 && b2 > a) return s.substring(a + 1, b2); }
        } catch (Exception ignored) { }
        String[] k = d.list();
        return k != null && k.length > 0 ? "installed" : "none";
    }
    String driverStatus() {
        String n = customDriverName();
        boolean sys = "system".equals(new Ini(new File(config, "graphics.ini")).get("driver", "custom"));
        return n.equals("none") || sys ? "phone's own" : n;
    }

    /* ------------------------------------------------------------ settings widgets */
    View choice(Ini ini, String key, String label, String[] vals, String[] names, String def, String help) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(10), 0, dp(4));
        row.addView(text(label, 15, TEXT, false));
        LinearLayout seg = new LinearLayout(this);
        seg.setPadding(0, dp(6), 0, 0);
        String cur = ini.get(key, def);
        boolean known = false;
        for (String v : vals) known |= v.equalsIgnoreCase(cur);
        if (!known && key.equals("diagnostics")) cur = def;      /* a value from an older version (diagnostics = auto) */
        for (int i = 0; i < vals.length; i++) {
            final String v = vals[i];
            TextView b = text(names[i], 13, v.equalsIgnoreCase(cur) ? Color.WHITE : DIM, true);
            b.setGravity(Gravity.CENTER);
            b.setPadding(dp(10), dp(8), dp(10), dp(8));
            b.setBackground(round(v.equalsIgnoreCase(cur) ? ACCENT2 : CARD2, 10));
            b.setOnClickListener(x -> { ini.set(key, v); show(tab); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
            lp.setMargins(i == 0 ? 0 : dp(4), 0, 0, 0);
            seg.addView(b, lp);
        }
        row.addView(seg);
        if (help != null) row.addView(text(help, 12, DIM, false));
        return row;
    }

    View slider(Ini ini, String key, String label, int min, int max, int step, int defPct, String unit, float scale, String help) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(10), 0, dp(4));
        int cur = Math.round(parse(ini.get(key, String.valueOf(defPct * scale)), defPct * scale) / scale);
        cur = Math.max(min, Math.min(max, cur));
        TextView title = text(label + ": " + cur + unit, 15, TEXT, false);
        row.addView(title);
        SeekBar sb = new SeekBar(this);
        sb.setMax((max - min) / step);
        sb.setProgress((cur - min) / step);
        sb.setProgressTintList(ColorStateList.valueOf(ACCENT2));
        sb.setThumbTintList(ColorStateList.valueOf(ACCENT2));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s, int p, boolean u) { title.setText(label + ": " + (min + p * step) + unit); }
            public void onStartTrackingTouch(SeekBar s) { }
            public void onStopTrackingTouch(SeekBar s) {
                float v = (min + s.getProgress() * step) * scale;
                ini.set(key, String.format(java.util.Locale.ROOT, "%.2f", v));
                show(tab);                          /* choices that name this value (presets) show whether they still match it */
            }
        });
        row.addView(sb);
        if (help != null) row.addView(text(help, 12, DIM, false));
        return row;
    }

    View toggle(Ini ini, String key, String label, String on, String off, String def, String help) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(8), 0, dp(4));
        Switch s = new Switch(this);
        s.setText(label);
        s.setTextColor(TEXT);
        s.setTextSize(15);
        s.setChecked(on.equalsIgnoreCase(ini.get(key, def)));
        s.setThumbTintList(ColorStateList.valueOf(Color.WHITE));
        s.setTrackTintList(ColorStateList.valueOf(ACCENT2));
        s.setOnCheckedChangeListener((x, c) -> ini.set(key, c ? on : off));
        row.addView(s);
        if (help != null) row.addView(text(help, 12, DIM, false));
        return row;
    }

    /* ------------------------------------------------------------ small views */
    LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(18), dp(16), dp(18), dp(16));
        c.setBackground(round(CARD, 16));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.setMargins(0, dp(6), 0, dp(10));
        c.setLayoutParams(lp);
        return c;
    }
    TextView header(String s) { TextView t = text(s.toUpperCase(), 12, ACCENT2, true); t.setLetterSpacing(0.08f); t.setPadding(0, 0, 0, dp(4)); return t; }
    TextView note(String s) { TextView t = text(s, 12, DIM, false); t.setPadding(dp(4), dp(4), dp(4), dp(4)); return t; }
    View statusLine(String k, String v, int col) {
        LinearLayout r = new LinearLayout(this);
        r.setPadding(0, dp(3), 0, dp(3));
        r.addView(text(k, 14, DIM, false), new LinearLayout.LayoutParams(0, -2, 1));
        r.addView(text(v, 14, col, true));
        return r;
    }
    /* The manifest's versionName (bumped for every build handed out; CHANGELOG.md): on the launcher and in exported logs. */
    String appVersion() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return "?"; }
    }
    TextView text(String s, int sp, int col, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(col);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }
    Button button(String s, int col) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(15);
        b.setBackground(round(col, 14));
        return b;
    }
    View space(int h) { View v = new View(this); v.setLayoutParams(new LinearLayout.LayoutParams(1, dp(h))); return v; }
    GradientDrawable round(int col, int r) { GradientDrawable g = new GradientDrawable(); g.setColor(col); g.setCornerRadius(dp(r)); return g; }
    static String size(int w, int h, float k) { return Math.round(w * k) + "x" + Math.round(h * k); }
    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
    void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }
    void confirm(String q, Runnable yes) { confirm(q, "Reset", yes); }
    void confirm(String q, String verb, Runnable yes) {
        new AlertDialog.Builder(this).setMessage(q).setPositiveButton(verb, (d, w) -> yes.run()).setNegativeButton("Cancel", null).show();
    }

    interface Work { void run() throws Exception; }
    void background(String what, Work w, Runnable done) {
        AlertDialog d = new AlertDialog.Builder(this).setMessage(what).setCancelable(false).show();
        new Thread(() -> {
            String err = null;
            try { w.run(); } catch (Exception e) { err = e.getMessage(); }
            final String fe = err;
            runOnUiThread(() -> { d.dismiss(); if (fe != null) toast("Failed: " + fe); done.run(); });
        }).start();
    }

    static float parse(String s, float d) { try { return Float.parseFloat(s.trim()); } catch (Exception e) { return d; } }
    static void pipe(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }
    static byte[] readAll(InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
        try (InputStream i = in) { pipe(i, o); }
        return o.toByteArray();
    }
    static void deleteTree(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) deleteTree(c);
        f.delete();
    }

    /* An ini file of the runtime: values read and set on their own lines, comments and order kept. */
    static class Ini {
        final File file;
        Ini(File f) { file = f; }
        List<String> lines() {
            List<String> l = new ArrayList<>();
            if (!file.exists()) return l;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
                for (String s; (s = r.readLine()) != null; ) l.add(s);
            } catch (Exception ignored) { }
            return l;
        }
        static String keyOf(String line) {
            String t = line.trim();
            if (t.startsWith(";") || !t.contains("=")) return null;
            return t.substring(0, t.indexOf('=')).trim();
        }
        String get(String key, String def) {
            for (String s : lines()) if (key.equalsIgnoreCase(keyOf(s))) return s.substring(s.indexOf('=') + 1).trim();
            return def;
        }
        void remove(String key) {
            List<String> l = lines();
            if (!l.removeIf(s -> key.equalsIgnoreCase(keyOf(s)))) return;
            write(l);
        }
        void write(List<String> l) {
            file.getParentFile().mkdirs();
            try (OutputStream o = new FileOutputStream(file)) {
                for (String s : l) o.write((s + "\r\n").getBytes(StandardCharsets.UTF_8));
            } catch (Exception ignored) { }
        }
        void set(String key, String value) {
            List<String> l = lines();
            boolean done = false;
            for (int i = 0; i < l.size(); i++) if (key.equalsIgnoreCase(keyOf(l.get(i)))) { l.set(i, key + " = " + value); done = true; }
            if (!done) l.add(key + " = " + value);
            write(l);
        }
    }
}
