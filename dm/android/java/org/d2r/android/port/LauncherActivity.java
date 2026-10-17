// 启动界面：选择设置与导入数据包，写进 launch-settings.conf 后再真正启动游戏页面。
// 构图照标题画面来做：美术铺满整屏，标题在右上，菜单是右侧一列，版本信息在最右下。
// Launcher: collects settings and imports a data package, writes them into launch-settings.conf, then starts the
// game page. The composition follows a title screen: artwork fills the screen, the logo sits top-right, the menu
// is a single right-aligned column, and app info sits at the bottom right.
package org.d2r.android.port;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class LauncherActivity extends Activity {
    // launch-settings.conf 由 native 严格解析（键白名单、失败即中止启动、且不接受任意路径），
    // 所以只有它认识的键才写进去；界面自己的状态另存一份，互不污染。
    // launch-settings.conf is parsed strictly by native (key whitelist, abort startup on failure, no arbitrary
    // paths allowed), so only keys it knows may go in. The launcher's own state lives in a separate file.
    private static final String CONFIG_NAME = "launch-settings.conf";
    private static final String STATE_NAME = "launcher-state.conf";

    // 导入的包解到这里，必须是 boot() 认的那个固定位置：它靠 manifest.json 是否存在决定用哪套数据。
    // The imported package unpacks here, the fixed location boot() looks for: it decides which data to use by
    // whether manifest.json is present.
    private static final String PACKAGE_DIRECTORY = "d2r-package";
    private static final String MANIFEST_NAME = "manifest.json";
    private static final String[] ALLOWED_PREFIXES = {"game/", "romfs/", "shaders/"};
    private static final int PICK_PACKAGE = 1;

    private static final int GOLD = 0xffe8c078;
    private static final int PARCHMENT = 0xffe8e0d0;
    private static final int MUTED = 0xffa2988c;
    private static final int WARNING = 0xffd08a78;

    private Switch overlaySwitch;
    private TextView profileItem, resolutionItem, importItem, statusView, startItem;
    private boolean handheld;
    private int resolutionIndex;
    private boolean importing;
    private volatile boolean cancelImport;
    // 出图分辨率只提供几档预设：它是"主机档"尺寸，掌机档由 native 按 0.8 同比例缩放（与 Mac/iOS 同一语义）。
    // 空字符串表示跟随档位（不写这个键，用原版 16:9 常数）。
    // The output resolution offers a few presets only: it is the docked-profile size and the handheld profile is
    // scaled by 0.8 inside native code, the same semantics as Mac and iOS. An empty string means "follow the
    // profile", which writes no key and keeps the original 16:9 constants.
    private static final String[] RESOLUTION_PRESETS = {"", "1920x1080", "2560x1440"};

    private String text(String key) {
        int resource = getResources().getIdentifier(key, "string", getPackageName());
        return resource == 0 ? key : getString(resource);
    }

    private File configFile() {
        File directory = new File(getFilesDir(), "temp");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IllegalStateException("cannot create " + directory);
        return new File(directory, CONFIG_NAME);
    }

    private File stateFile() { return new File(getFilesDir(), STATE_NAME); }

    // 大资源解在外部私有目录：那里放得下 9 GB 级的数据，内部数据目录通常放不下。
    // 但**模块必须解在内部目录**：外部存储是 noexec 挂载（实测 /storage/emulated 带 noexec），
    // 从那里 dlopen 会失败；内部目录没有 noexec，图形驱动就是从内部目录加载的。
    // Big assets go to the external private directory, which has room for multi-gigabyte data. The modules must
    // go to the internal one: external storage is mounted noexec (measured on /storage/emulated), so dlopen from
    // there fails, while the internal directory has no such flag — the graphics driver already loads from it.
    private File packageDirectory() {
        File external = getExternalFilesDir(null);
        return external == null ? new File(getFilesDir(), PACKAGE_DIRECTORY) : new File(external, PACKAGE_DIRECTORY);
    }

    private File packageGameDirectory() { return new File(getFilesDir(), PACKAGE_DIRECTORY + "/game"); }

    private File packageManifest() { return new File(packageDirectory(), MANIFEST_NAME); }

    // 导入是否完成，以 manifest.json 为准（它在解包最后才写）。没导入就不该能启动游戏。
    // "Imported" means manifest.json exists, and it is written last. Without it the game must not start.
    private boolean packageReady() { return packageManifest().isFile(); }

    // 只做最简单的 key=value 读取；权威解析在 native 的 launch_settings.c。
    // Minimal key=value read; the authoritative parser is launch_settings.c on the native side.
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

    private void write(File file, String content) {
        try (OutputStreamWriter writer = new OutputStreamWriter(
                new FileOutputStream(file, false), StandardCharsets.UTF_8)) {
            writer.write(content);
        } catch (IOException error) {
            setStatus(error.getMessage(), WARNING);
        }
    }

    private void setStatus(String message, int colour) {
        statusView.setTextColor(colour);
        statusView.setText(message == null ? "" : message);
    }

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
        return view;
    }

    private String profileLabel() {
        return text("dmAndroidLaunchProfile") + "：" +
            text(handheld ? "dmAndroidLaunchHandheld" : "dmAndroidLaunchConsole");
    }

    private String resolutionLabel() {
        String preset = RESOLUTION_PRESETS[resolutionIndex];
        if (preset.isEmpty()) return text("dmAndroidLaunchResolution") + "：" + text("dmAndroidLaunchFollowProfile");
        String[] parts = preset.split("x");
        return text("dmAndroidLaunchResolution") + "：" + parts[0] + "×" + parts[1];
    }

    private String importLabel() {
        String summary = read(stateFile(), "package_summary");
        return summary == null ? text("dmAndroidLaunchImport") + "：" + text("dmAndroidLaunchNotSet")
                               : String.format(text("dmAndroidLaunchImported"), summary);
    }

    // 没有数据包就把"启动游戏"置灰：点了会失败的事不该看起来能点。
    // Without a package the start entry greys out: something that would fail should not look available.
    private void updateStartState() {
        boolean ready = packageReady() && !importing;
        startItem.setEnabled(ready);
        startItem.setTextColor(ready ? GOLD : MUTED);
    }

    // 把当前内存里的设置反映到界面上。点一下之后只调这个，**绝不重新读盘** ——
    // 读盘会把刚改的内存值覆盖回去，结果就是点了没反应（分辨率与画面档位两行都曾如此）。
    // Reflect the in-memory settings onto the labels. Click handlers call only this and never re-read from disk:
    // reloading overwrites the value just changed, so tapping did nothing (true for both the resolution and the
    // profile row).
    private void applyLabels() {
        if (profileItem != null) profileItem.setText(profileLabel());
        if (resolutionItem != null) resolutionItem.setText(resolutionLabel());
        if (importItem != null) importItem.setText(importLabel());
        if (startItem != null) updateStartState();
    }

    // 从盘重读全部设置到内存，再刷新界面。只在进入/回到界面时调。
    // Read every setting from disk into memory, then refresh the labels. Called only when the screen is (re)entered.
    private void refreshFromDisk() {
        handheld = !"console".equals(read(configFile(), "operation_mode"));
        if (overlaySwitch != null) overlaySwitch.setChecked(!"0".equals(read(configFile(), "debug_overlay")));
        String preset = read(configFile(), "resolution");
        resolutionIndex = 0;
        if (preset != null) for (int i = 0; i < RESOLUTION_PRESETS.length; i++) if (RESOLUTION_PRESETS[i].equals(preset)) resolutionIndex = i;
        applyLabels();
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

        column.addView(item(text("dmAndroidLaunchTitle"), 42, GOLD, true, false));
        column.addView(item(text("dmAndroidLaunchSubtitle"), 15, MUTED, false, false));

        column.addView(new View(this), new LinearLayout.LayoutParams(1, 0, 1.6f));

        startItem = item("▸  " + text("dmAndroidLaunchStart"), 28, GOLD, true, true);
        startItem.setOnClickListener(v -> startGame());
        column.addView(startItem);

        overlaySwitch = new Switch(this);
        overlaySwitch.setText(text("dmAndroidLaunchOverlay"));
        overlaySwitch.setTextColor(PARCHMENT);
        overlaySwitch.setTextSize(19);
        overlaySwitch.setGravity(Gravity.END);
        column.addView(overlaySwitch);

        profileItem = item(profileLabel(), 19, PARCHMENT, false, true);
        profileItem.setOnClickListener(v -> { handheld = !handheld; applyLabels(); });
        column.addView(profileItem);

        resolutionItem = item(resolutionLabel(), 19, PARCHMENT, false, true);
        resolutionItem.setOnClickListener(v -> { resolutionIndex = (resolutionIndex + 1) % RESOLUTION_PRESETS.length; applyLabels(); });
        column.addView(resolutionItem);

        importItem = item(importLabel(), 19, PARCHMENT, false, true);
        importItem.setOnClickListener(v -> pickPackage());
        column.addView(importItem);

        column.addView(new View(this), new LinearLayout.LayoutParams(1, 0, 1f));

        statusView = item("", 14, WARNING, false, false);
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

    // 选包用系统文件选择器：应用不需要任何存储权限，也不必知道文件放在哪。
    // The system picker chooses the package, so the app needs no storage permission and does not care where the
    // file lives. Selecting it again while an import runs asks that import to stop.
    private void pickPackage() {
        if (importing) {
            cancelImport = true;
            setStatus(text("dmAndroidLaunchCancel"), MUTED);
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // 部分提供方把 zip 报成 octet-stream，所以类型放宽；否则文件在选择器里是灰的。
        // Some providers report a zip as octet-stream, so the type is left open; otherwise the file greys out.
        intent.setType("*/*");
        try {
            startActivityForResult(intent, PICK_PACKAGE);
        } catch (RuntimeException error) {
            setStatus(String.format(text("dmAndroidLaunchImportFailed"), String.valueOf(error.getMessage())), WARNING);
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_PACKAGE || result != RESULT_OK || data == null || data.getData() == null) return;
        beginImport(data.getData());
    }

    private void beginImport(Uri uri) {
        importing = true;
        cancelImport = false;
        updateStartState();
        setStatus(String.format(text("dmAndroidLaunchImporting"), 0), MUTED);
        new Thread(() -> {
            try {
                String summary = extract(uri, documentSize(uri));
                runOnUiThread(() -> {
                    write(stateFile(), "# 导入结果，仅供界面显示 | import result, display only\npackage_summary=" + summary + "\n");
                    refreshFromDisk();
                    setStatus(summary, PARCHMENT);
                });
            } catch (Exception error) {
                String message = String.valueOf(error.getMessage());
                runOnUiThread(() -> setStatus(String.format(text("dmAndroidLaunchImportFailed"), message), WARNING));
            } finally {
                importing = false;
                runOnUiThread(this::updateStartState);
            }
        }, "d2r-package-import").start();
    }

    private long documentSize(Uri uri) {
        try (android.database.Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) return cursor.getLong(0);
        } catch (RuntimeException ignored) {
        }
        return -1;
    }

    // 解包顺序是刻意的：旧的 manifest.json 先删，新的最后才落盘。boot() 用"manifest.json 在不在"判断包能不能用，
    // 所以中途失败（空间不足、取消、zip 损坏）留下的是个没有 manifest 的目录，游戏会回落到默认数据，而不是读到半个包。
    // The order is deliberate: the previous manifest.json is removed first and the new one written last. boot()
    // treats "manifest.json present" as "package usable", so an interrupted import (no space, cancelled, corrupt
    // zip) leaves a directory without a manifest and the game falls back to the default data rather than reading
    // half a package.
    private String extract(Uri uri, long totalBytes) throws IOException {
        File assets = packageDirectory();          // romfs / shaders / manifest.json
        File modules = packageGameDirectory();     // game/：必须在内部目录，外部是 noexec
        File manifest = packageManifest();
        if (manifest.isFile() && !manifest.delete()) throw new IOException("cannot clear " + manifest);
        deleteTree(modules);                       // 上一次导入的模块不能留下来跟新包混在一起
        byte[] manifestBytes = null;
        int romfs = 0, shaders = 0, game = 0;
        byte[] buffer = new byte[1 << 20];
        long written = 0, lastReported = 0;
        try (InputStream raw = getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("cannot open the selected file");
            try (ZipInputStream zip = new ZipInputStream(raw)) {
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    if (cancelImport) throw new IOException("cancelled");
                    String name = entry.getName();
                    if (entry.isDirectory()) continue;
                    if (name.equals(MANIFEST_NAME)) {
                        // 读进内存，最后再落盘；顺序说明见函数上方。
                        // Kept in memory and written last; see the ordering note above the function.
                        manifestBytes = readAll(zip);
                        continue;
                    }
                    if (!allowed(name)) throw new IOException("unexpected entry in package: " + name);
                    // game/ 剥掉前缀后落到内部目录，其余原样落到外部目录。
                    // game/ drops its prefix and lands in the internal directory; everything else goes external.
                    File destination = name.startsWith("game/")
                        ? safeDestination(modules, name.substring("game/".length()))
                        : safeDestination(assets, name);
                    File parent = destination.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
                    written += writeFile(zip, destination, buffer);
                    if (name.startsWith("romfs/")) romfs++;
                    else if (name.startsWith("shaders/")) shaders++;
                    else game++;
                    if (totalBytes > 0 && written - lastReported > totalBytes / 100) {
                        lastReported = written;
                        final int percent = (int)Math.min(100, written * 100 / totalBytes);
                        runOnUiThread(() -> setStatus(String.format(text("dmAndroidLaunchImporting"), percent), MUTED));
                    }
                }
            }
        }
        if (manifestBytes == null) throw new IOException("package has no " + MANIFEST_NAME);
        if (romfs == 0 || shaders == 0 || game == 0) throw new IOException("package is incomplete");
        String summary = String.format(text("dmAndroidLaunchImportSummary"),
                                       counts(manifestBytes, "romfs", romfs), counts(manifestBytes, "shaders", shaders));
        try (OutputStream out = new FileOutputStream(manifest, false)) {
            out.write(manifestBytes);
        }
        return summary;
    }

    // 包自报的条目数与实际解出的数量不一致就说不一致，不拿其中一个当"结果"糊过去。
    // If the package's own count disagrees with what was unpacked, say so instead of quietly picking one.
    private int counts(byte[] manifest, String key, int extracted) {
        try {
            return new JSONObject(new String(manifest, StandardCharsets.UTF_8)).getJSONObject("counts").getInt(key);
        } catch (Exception ignored) {
            return extracted;
        }
    }

    private static boolean allowed(String name) {
        for (String prefix : ALLOWED_PREFIXES) if (name.startsWith(prefix)) return true;
        return false;
    }

    // 防 zip 内路径穿越：规范化后必须仍在目标目录之内，否则拒绝。
    // Guards against path traversal inside the zip: the normalised path must still sit under the target.
    private static File safeDestination(File target, String name) throws IOException {
        File candidate = new File(target, name);
        String root = target.getCanonicalPath() + File.separator;
        if (!candidate.getCanonicalPath().startsWith(root)) throw new IOException("package entry escapes the target directory: " + name);
        return candidate;
    }

    // 递归删除一棵目录树；只用于应用自己的包目录，不碰别处。
    // Recursive delete, used only on the app's own package directory.
    private static void deleteTree(File target) {
        File[] children = target.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        if (target.exists() && !target.delete()) target.deleteOnExit();
    }

    private static byte[] readAll(InputStream source) throws IOException {
        ByteArrayOutputStream collected = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = source.read(buffer)) > 0) collected.write(buffer, 0, count);
        return collected.toByteArray();
    }

    private static long writeFile(InputStream source, File destination, byte[] buffer) throws IOException {
        long written = 0;
        try (OutputStream out = new FileOutputStream(destination, false)) {
            int count;
            while ((count = source.read(buffer)) > 0) { out.write(buffer, 0, count); written += count; }
        }
        return written;
    }

    private void startGame() {
        if (importing) {
            setStatus(String.format(text("dmAndroidLaunchImporting"), 0), WARNING);
            return;
        }
        if (!packageManifest().isFile()) {
            setStatus(text("dmAndroidLaunchNeedsImport"), WARNING);
            return;
        }
        StringBuilder content = new StringBuilder();
        content.append("# 由启动界面写入 | written by the launcher\n");
        content.append("debug_overlay=").append(overlaySwitch.isChecked() ? "1" : "0").append('\n');
        content.append("operation_mode=").append(handheld ? "handheld" : "console").append('\n');
        // 跟随档位时不写这个键：native 语义里"省略"就是不改动，保持原版 16:9 常数。
        // "Follow profile" writes no key: omitting it means "leave it alone" to native, keeping the original 16:9 constants.
        if (!RESOLUTION_PRESETS[resolutionIndex].isEmpty()) content.append("resolution=").append(RESOLUTION_PRESETS[resolutionIndex]).append('\n');
        write(configFile(), content.toString());
        setStatus("", PARCHMENT);
        startActivity(new Intent(this, D2RActivity.class));
    }
}
