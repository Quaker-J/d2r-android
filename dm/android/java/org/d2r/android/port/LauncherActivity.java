// 启动界面：进入游戏之前收集设置，写进 launch-settings.conf 后再真正启动游戏页面。
// 构图照标题画面来做：美术铺满整屏，标题在右上，菜单是右侧一列，版本信息在最右下。
// Launcher: collects settings before play, writes them into launch-settings.conf, then starts the game page.
// The composition follows a title screen: artwork fills the screen, the logo sits top-right, the menu is a single
// right-aligned column, and version/app info sits at the bottom right.
package org.d2r.android.port;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;

public final class LauncherActivity extends Activity {
    // 两个文件分开是刻意的：launch-settings.conf 由 native 严格解析（键白名单、失败即中止启动、
    // 并且不允许指定任意路径），所以只有它认识的键才能写进去；界面自己的状态另存一份，互不污染。
    // The two files are deliberately separate: launch-settings.conf is parsed strictly by native (key whitelist,
    // abort startup on failure, no arbitrary paths allowed), so only keys it knows may go in. The launcher's own
    // state lives in a second file, so the two cannot contaminate each other.
    private static final String CONFIG_NAME = "launch-settings.conf";
    private static final String STATE_NAME = "launcher-state.conf";

    private static final int GOLD = 0xffe8c078;
    private static final int PARCHMENT = 0xffe8e0d0;
    private static final int MUTED = 0xffa2988c;

    private Switch overlaySwitch;
    private TextView profileItem, gameFilesItem, keyFileItem, statusView;
    private boolean handheld;

    // 项目没有 Gradle，也就不生成 R.java，所以文字只能用键名查表（与 TouchControls 同一做法）。
    // There is no Gradle build and no generated R.java, so text is looked up by key (same approach as TouchControls).
    private String text(String key) {
        int resource = getResources().getIdentifier(key, "string", getPackageName());
        return resource == 0 ? key : getString(resource);
    }

    // 与 native 侧同一个目录：<internalDataPath>/temp。boot() 自己也会 mkdir，这里先建好以免首次启动写失败。
    // Same directory as the native side, <internalDataPath>/temp; boot() also mkdirs it, done here so the first write cannot fail.
    private File configFile() {
        File directory = new File(getFilesDir(), "temp");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IllegalStateException("cannot create " + directory);
        return new File(directory, CONFIG_NAME);
    }

    private File stateFile() { return new File(getFilesDir(), STATE_NAME); }

    // 这里只做最简单的 key=value 读取，用来把当前设置显示出来。
    // 权威解析在 native（launch_settings.c）：键白名单、重复键、非法值、失败不半应用都由它负责。
    // Minimal key=value read, only to display the current settings. The authoritative parser is native
    // (launch_settings.c): the key whitelist, duplicates, invalid values and no-partial-apply are its job.
    private String read(File file, String key) {
        if (!file.isFile()) return null;
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int separator = line.indexOf('=');
                if (separator <= 0) continue;
                if (line.substring(0, separator).trim().equals(key)) return line.substring(separator + 1).trim();
            }
        } catch (IOException ignored) {
        }
        return null;
    }

    private boolean write(File file, String content) {
        try (OutputStreamWriter writer = new OutputStreamWriter(
                new FileOutputStream(file, false), StandardCharsets.UTF_8)) {
            writer.write(content);
            return true;
        } catch (IOException error) {
            statusView.setText(error.getMessage());
            return false;
        }
    }

    // 右侧那一列菜单项：右对齐、无边框、点哪一项就执行哪一项，跟标题画面一致。
    // The right-hand menu column: right-aligned, chrome-free, each entry acts when tapped, as on a title screen.
    private TextView item(String value, float size, int colour, boolean bold, boolean clickable) {
        float density = getResources().getDisplayMetrics().density;
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextColor(colour);
        view.setTextSize(size);
        view.setGravity(Gravity.END);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setPadding(0, (int)(7 * density), 0, (int)(7 * density));
        view.setClickable(clickable);
        if (clickable) view.setBackgroundColor(0x00000000);
        return view;
    }

    private String profileLabel() {
        return text("dmAndroidLaunchProfile") + "：" +
            text(handheld ? "dmAndroidLaunchHandheld" : "dmAndroidLaunchConsole");
    }

    // 每一帧回到界面都按磁盘上的实际值刷新，避免显示和真正生效的设置不一致。
    // Refresh from disk every time the screen comes back so the display cannot drift from what takes effect.
    private void refreshFromDisk() {
        handheld = !"console".equals(read(configFile(), "operation_mode"));
        if (overlaySwitch != null) overlaySwitch.setChecked(!"0".equals(read(configFile(), "debug_overlay")));
        if (profileItem != null) profileItem.setText(profileLabel());
        String gameFiles = read(stateFile(), "game_files");
        if (gameFilesItem != null) gameFilesItem.setText(text("dmAndroidLaunchGameFiles") + "：" +
            (gameFiles == null ? text("dmAndroidLaunchNotSet") : gameFiles));
        String keyFile = read(stateFile(), "key_file");
        if (keyFileItem != null) keyFileItem.setText(text("dmAndroidLaunchKeys") + "：" +
            (keyFile == null ? text("dmAndroidLaunchNotSet") : keyFile));
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int width = getResources().getDisplayMetrics().widthPixels;
        int height = getResources().getDisplayMetrics().heightPixels;
        int margin = (int)(width * 0.045f);

        FrameLayout root = new FrameLayout(this);
        root.addView(new LauncherBackgroundView(this),
            new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.END);
        column.setPadding(margin, (int)(height * 0.075f), margin, (int)(height * 0.045f));

        TextView title = item(text("dmAndroidLaunchTitle"), 42, GOLD, true, false);
        column.addView(title);
        column.addView(item(text("dmAndroidLaunchSubtitle"), 15, MUTED, false, false));

        // 上下的空白权重把菜单推到画面中部偏下，跟标题画面同一节奏。
        // The weighted spacers push the menu to just below centre, matching a title screen's rhythm.
        column.addView(new View(this), new LinearLayout.LayoutParams(1, 0, 1.6f));

        TextView start = item("▸  " + text("dmAndroidLaunchStart"), 28, GOLD, true, true);
        start.setOnClickListener(v -> startGame());
        column.addView(start);

        overlaySwitch = new Switch(this);
        overlaySwitch.setText(text("dmAndroidLaunchOverlay"));
        overlaySwitch.setTextColor(PARCHMENT);
        overlaySwitch.setTextSize(19);
        overlaySwitch.setGravity(Gravity.END);
        column.addView(overlaySwitch);

        profileItem = item(profileLabel(), 19, PARCHMENT, false, true);
        profileItem.setOnClickListener(v -> { handheld = !handheld; refreshFromDisk(); });
        column.addView(profileItem);

        // 尚未接入的两项：如实显示“未设置”，不做成点了没反应的按钮。
        // Not wired up yet: they show "not set" rather than pretending to be controls.
        gameFilesItem = item("", 17, MUTED, false, false);
        column.addView(gameFilesItem);
        keyFileItem = item("", 17, MUTED, false, false);
        column.addView(keyFileItem);

        column.addView(new View(this), new LinearLayout.LayoutParams(1, 0, 1f));

        statusView = item("", 14, 0xffd08a78, false, false);
        column.addView(statusView);
        column.addView(item(text("dmAndroidLaunchPending"), 13, MUTED, false, false));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(column, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                            ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(scroll, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                          ViewGroup.LayoutParams.MATCH_PARENT));

        refreshFromDisk();
        setContentView(root);
    }

    @Override protected void onResume() {
        super.onResume();
        refreshFromDisk();
    }

    // 只写 native 认识的那两个键，且写全（省略的键在 native 语义里等于“不改动”）。
    // Only the two keys native knows, written in full: an omitted key means "leave it alone" to native.
    private void startGame() {
        StringBuilder content = new StringBuilder();
        content.append("# 由启动界面写入 | written by the launcher\n");
        content.append("debug_overlay=").append(overlaySwitch.isChecked() ? "1" : "0").append('\n');
        content.append("operation_mode=").append(handheld ? "handheld" : "console").append('\n');
        if (!write(configFile(), content.toString())) return;
        statusView.setText("");
        startActivity(new Intent(this, D2RActivity.class));
    }
}
