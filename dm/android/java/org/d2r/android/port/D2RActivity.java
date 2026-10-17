// 原生游戏窗口与 Android 文本输入；游戏线程等待结果，主线程保持响应。
// Native game window and Android text entry; the guest waits for a result while the UI remains responsive.
package org.d2r.android.port;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.input.InputManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.TextWatcher;
import android.text.InputType;
import android.view.WindowManager;
import android.view.Display;
import android.view.InputDevice;
import android.util.Log;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
import java.io.File;
import android.view.Gravity;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import java.nio.charset.StandardCharsets;

public final class D2RActivity extends NativeActivity {
    private AlertDialog textDialog;
    private long textRequest;
    private TextView debugView;
    private PopupWindow debugPopup;
    private TouchControls touchControls;
    private boolean activityResumed;
    private BroadcastReceiver batteryReceiver;
    private volatile int batteryVoltageMv = -1;
    private volatile int batteryLevel = -1;
    private volatile int batteryStatus = -1;
    private boolean physicalController;
    // 同一进程里「游戏活动是否还活着」的标记，启动界面据此决定让位（见 LauncherActivity.stepAsideForRunningGame）。
    // 一个进程只跑一个游戏，所以它也等于「这一局还在」。
    // Same-process flag: is the game activity still alive. The launcher uses it to decide whether to step aside (see
    // LauncherActivity.stepAsideForRunningGame). One process hosts one guest, so it also means "this session is live".
    static volatile boolean hostRunning;
    private static native void onTextResult(long request, String text, boolean accepted);
    private static native void onTouchPad(long buttons, int leftX, int leftY, int rightX, int rightY);
    private static native void setOverlayEnabled(boolean enabled);
    private static native void setQualityPreset(int preset);
    private static native void setMenuOpen(boolean open);

    // ---------- 游戏内菜单（返回键呼出，照 Eden 那样）----------
    // 返回键在安卓上默认结束当前页面，而本移植里结束游戏页面 = 结束整个进程（一个进程只跑一个游戏）；
    // 横屏gesture导航下"边缘内滑"极易误触，一下就整局退出。所以返回键改成呼出这个菜单：
    // 帧率显示 / 虚拟摇杆 / 画质 / 退出游戏。再按一次返回（或点菜单外面）收起。
    // The in-game menu, opened with the system back key (as Eden does). Back ends the page by default, and here
    // ending the game page ends the whole process (one guest per process), while an edge swipe in landscape is very
    // easy to hit by accident and would quit the whole run. So back opens this menu instead: frame rate display /
    // on-screen controls / quality / exit game. Press back again (or tap outside) to close it.
    private PopupWindow menuPopup;
    private TextView menuOverlayRow, menuControlsRow, menuQualityRow;
    private boolean overlayEnabled = true;
    private boolean controlsEnabled = true;
    private int qualityPreset;                                  // 0 自动 / 1 高质量 / 2 平衡 / 3 性能 | 0 auto / 1 high / 2 balanced / 3 performance
    private static final int[] QUALITY_LEVELS = {-1, 0, 1, 2};   // 与 native 约定：-1 自动、0 高、1 平衡、2 性能 | the native contract
    private static final String[] QUALITY_KEYS = {"dmAndroidMenuQualityAuto", "dmAndroidMenuQualityHigh", "dmAndroidMenuQualityBalanced", "dmAndroidMenuQualityPerformance"};
    // 与启动界面同一套配色（那边是 GOLD/PARCHMENT），菜单里照用，视觉上是一家人。
    // The same palette as the launcher (GOLD/PARCHMENT there), so the menu looks like part of the same app.
    private static final int GOLD = 0xffe8c078;
    private static final int PARCHMENT = 0xffe8e0d0;

    private String text(String key) {
        int resource = getResources().getIdentifier(key, "string", getPackageName());
        return resource == 0 ? key : getString(resource);
    }

    private File configFile() {
        File directory = new File(getFilesDir(), "temp");
        if (!directory.isDirectory() && !directory.mkdirs()) return null;
        return new File(directory, "launch-settings.conf");
    }

    @Override public void onBackPressed() {
        if (menuPopup != null && menuPopup.isShowing()) hideMenu(); else showMenu();
    }

    private void showMenu() {
        if (menuPopup == null) buildMenu();
        updateMenuLabels();
        setMenuOpen(true);
        menuPopup.showAtLocation(getWindow().getDecorView(), Gravity.END | Gravity.CENTER_VERTICAL, 0, 0);
    }

    private void hideMenu() {
        if (menuPopup != null && menuPopup.isShowing()) menuPopup.dismiss();
        setMenuOpen(false);   // 菜单收起就放开输入，免得菜单关掉后手柄还按不动 | releasing the input block on close
    }

    private TextView menuRow() {
        float density = getResources().getDisplayMetrics().density;
        TextView view = new TextView(this);
        view.setTextColor(PARCHMENT);
        view.setTextSize(17);
        view.setPadding((int)(10 * density), (int)(9 * density), (int)(10 * density), (int)(9 * density));
        view.setClickable(true);
        return view;
    }

    private void buildMenu() {
        float density = getResources().getDisplayMetrics().density;
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setBackgroundColor(0xf0131317);
        column.setPadding((int)(20 * density), (int)(16 * density), (int)(20 * density), (int)(16 * density));
        TextView title = new TextView(this);
        title.setText(text("dmAndroidMenuTitle"));
        title.setTextColor(GOLD);
        title.setTextSize(19);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding((int)(10 * density), 0, (int)(10 * density), (int)(4 * density));
        column.addView(title);
        menuOverlayRow = menuRow();
        menuOverlayRow.setOnClickListener(v -> { overlayEnabled = !overlayEnabled; setOverlayEnabled(overlayEnabled); persistOverlay(); updateMenuLabels(); });
        column.addView(menuOverlayRow);
        menuControlsRow = menuRow();
        menuControlsRow.setOnClickListener(v -> { controlsEnabled = !controlsEnabled; updateTouchVisibility(); updateMenuLabels(); });
        column.addView(menuControlsRow);
        menuQualityRow = menuRow();
        menuQualityRow.setOnClickListener(v -> { qualityPreset = (qualityPreset + 1) % QUALITY_KEYS.length; setQualityPreset(QUALITY_LEVELS[qualityPreset]); updateMenuLabels(); });
        column.addView(menuQualityRow);
        TextView exitRow = menuRow();
        exitRow.setTextColor(GOLD);
        exitRow.setText(text("dmAndroidMenuExit"));
        // 退出走 finish()：与按返回键退出同一条路（先请游戏存盘、再释放窗口、再结束进程）。
        // Exit goes through finish(): the same path the back key used to take (ask the game to save, release the
        // window, then end the process).
        exitRow.setOnClickListener(v -> { hideMenu(); finish(); });
        column.addView(exitRow);
        menuPopup = new PopupWindow(column, LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT, true);
        // 透明背景 + 可点外面：点菜单以外的地方就收起；焦点交给菜单，这样返回键先被菜单吃掉。
        // Transparent background plus outside-touchable: tapping elsewhere closes it, and giving the popup focus is
        // what lets it consume the back key first.
        menuPopup.setBackgroundDrawable(new ColorDrawable(0));
        menuPopup.setOutsideTouchable(true);
        menuPopup.setOnDismissListener(() -> setMenuOpen(false));
    }

    private void updateMenuLabels() {
        if (menuOverlayRow == null) return;
        String on = text("dmAndroidMenuOn"), off = text("dmAndroidMenuOff");
        menuOverlayRow.setText(text("dmAndroidMenuOverlay") + "：" + (overlayEnabled ? on : off));
        menuControlsRow.setText(text("dmAndroidMenuControls") + "：" + (controlsEnabled ? on : off));
        menuQualityRow.setText(text("dmAndroidMenuQuality") + "：" + text(QUALITY_KEYS[qualityPreset]));
    }

    // 「帧率显示」写回配置：重启后还记得（native 启动时读 DM_DEBUG_OVERLAY）。
    // "Frame rate display" is written back so a restart remembers it (native reads DM_DEBUG_OVERLAY at startup).
    private void persistOverlay() {
        File file = configFile();
        if (file != null) LaunchSettings.write(file, "debug_overlay", overlayEnabled ? "1" : "0");
    }

    // 电池读数由框架提供（system_server 读内核节点、经 Binder 返回），应用不需要 root；
    // 直接读 /sys/class/power_supply/battery/current_now 才会被 SELinux 拒绝。
    // Battery readings come from the framework (system_server reads the kernel node and returns it over Binder),
    // so the app needs no root; reading /sys/class/power_supply/battery/current_now directly is SELinux-denied.
    // 只做只读查询：不请求频率、不改变游戏或显示节拍。
    // Read-only queries only: no rate requests, no change to game or display pacing.
    public int batteryCurrentMa() {
        BatteryManager manager = (BatteryManager) getSystemService(Context.BATTERY_SERVICE);
        if (manager == null) return Integer.MIN_VALUE;
        int raw = manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
        if (raw == 0 || raw == Integer.MIN_VALUE)
            raw = manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE);
        if (raw == 0 || raw == Integer.MIN_VALUE) return Integer.MIN_VALUE;
        // API 契约单位是微安，但实测本机（OPD2409 / Adreno 830）
        // `cmd battery get current_now` = 2791，同期电池计实测均 2350 mA，数量级一致，说明本机 HAL 给的是毫安。
        // 用数量级判单位而不是按机型硬编码：毫安读数不可能达到 20 A，微安读数在负载下不会低于 20 mA。
        // The API contract says microamps, but on this device (OPD2409 / Adreno 830) `cmd battery get current_now`
        // returned 2791 while the fuel gauge measured 2350 mA average, so this HAL reports milliamps. Decide by
        // magnitude rather than by device model: a milliamp reading cannot reach 20 A, and a microamp reading
        // under load is never below 20 mA.
        long magnitude = Math.abs((long) raw);
        if (magnitude >= 20000) magnitude /= 1000;
        return magnitude > Integer.MAX_VALUE ? Integer.MIN_VALUE : (int) magnitude;
    }

    public int batteryVoltageMv() { return batteryVoltageMv; }
    public int batteryLevelPercent() { return batteryLevel; }
    public int batteryStatusValue() { return batteryStatus; }

    // 只读真实屏幕支持的最高刷新率；不请求新模式、不改变游戏或显示节拍。
    // Read the display's actual highest supported refresh rate without requesting a mode or changing pacing.
    @SuppressWarnings("deprecation")
    public float getMaximumDisplayRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        if (display == null) return 0;
        float maximum = 0;
        for (Display.Mode mode : display.getSupportedModes()) {
            float rate = mode.getRefreshRate();
            if (Float.isFinite(rate) && rate > maximum) maximum = rate;
        }
        if (maximum == 0) {
            float rate = display.getRefreshRate();
            if (Float.isFinite(rate) && rate > 0) maximum = rate;
        }
        return maximum;
    }

    // 只修改本窗口偏好；匹配当前物理分辨率，保持游戏渲染分辨率。
    // Change only this window's preference, matching its current physical resolution.
    @SuppressWarnings("deprecation")
    public void requestDisplayFrameRate(float requested) {
        if (!Float.isFinite(requested) || requested < 0) return;
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) return;
            try {
                Display display = getWindowManager().getDefaultDisplay();
                Display.Mode current = display == null ? null : display.getMode();
                int selected = 0;
                float bestDifference = Float.MAX_VALUE;
                if (requested > 0 && current != null) {
                    for (Display.Mode mode : display.getSupportedModes()) {
                        if (mode.getPhysicalWidth() != current.getPhysicalWidth() ||
                            mode.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                        float difference = Math.abs(mode.getRefreshRate() - requested);
                        // 仅容忍浮点周期换算误差，不把邻近但不同的刷新率当作目标。
                        // Tolerate period conversion rounding, not a distinct nearby refresh rate.
                        if (difference <= 0.01f && (difference < bestDifference ||
                            (difference == bestDifference && mode.getModeId() == current.getModeId()))) {
                            selected = mode.getModeId();
                            bestDifference = difference;
                        }
                    }
                }
                WindowManager.LayoutParams attributes = getWindow().getAttributes();
                attributes.preferredDisplayModeId = selected;
                attributes.preferredRefreshRate = requested;
                getWindow().setAttributes(attributes);
                Log.i("D2RAndroid", "[android-display-preference] requested=" + requested +
                    " modeId=" + selected + " previousMode=" + (current == null ? 0 : current.getModeId()) +
                    " physical=" + (current == null ? "unknown" : current.getPhysicalWidth() + "x" + current.getPhysicalHeight()) +
                    " window preference only; actual refresh requires observation");
            } catch (RuntimeException error) {
                Log.e("D2RAndroid", "[android-display-preference] request failed", error);
            }
        });
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // 标记与活动的「创建 / 销毁」这一对绑定，语义就是"这个进程里还住着一个游戏活动"。
        // The flag is paired with activity creation/destruction: it means "a game activity still lives in this process".
        hostRunning = true;
        // 菜单里「帧率显示」的当前值：与 native 启动时读的是同一个键，**缺省关闭**（只有明确写 1 才是开）。
        // The menu's "frame rate display" starts from the same key native reads at startup; **off by default**
        // (only an explicit 1 means on).
        String overlaySetting = LaunchSettings.read(configFile(), "debug_overlay");
        overlayEnabled = overlaySetting != null && overlaySetting.equals("1");
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        debugView = new TextView(this);
        debugView.setTypeface(Typeface.MONOSPACE);
        debugView.setTextSize(12);
        debugView.setTextColor(0xffe4dcc9);
        debugView.setBackgroundColor(0xa6000000);
        debugView.setClickable(false);
        debugView.setFocusable(false);
        debugView.setImportantForAccessibility(android.view.View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        int padding = (int)(8 * getResources().getDisplayMetrics().density);
        debugView.setPadding(padding, padding, padding, padding);
        // Vulkan 拥有主窗口缓冲；独立应用内窗口才能稳定叠加系统绘制的文字。
        // Vulkan owns the main window buffer; a separate app window keeps system-drawn text visible above it.
        debugPopup = new PopupWindow(debugView, FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, false);
        debugPopup.setTouchable(false);
        debugPopup.setTouchModal(false);
        debugPopup.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        debugPopup.setBackgroundDrawable(new ColorDrawable(0));
        getWindow().getDecorView().post(() -> { if (!isFinishing() && !isDestroyed()) debugPopup.showAtLocation(getWindow().getDecorView(), Gravity.TOP | Gravity.LEFT, padding, padding); });
        // 粘性广播：注册即回调一次当前值，之后只在系统电量变化时推送，不产生轮询开销。
        // Sticky broadcast: registering delivers the current value once, then only on system battery changes; no polling.
        batteryReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                batteryVoltageMv = intent.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1);
                int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 0);
                batteryLevel = (level < 0 || scale <= 0) ? -1 : (int)Math.round(level * 100.0 / scale);
                batteryStatus = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            }
        };
        registerReceiver(batteryReceiver, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        // 设备监听只在变化时回调，所以要先用一次扫描拿到"启动时就已接入"的手柄。
        // The listener only fires on change, so scan once to catch a controller already connected at launch.
        InputManager inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(inputDeviceListener, null);
        refreshPhysicalController();
        touchControls = new TouchControls(this, (buttons, x, y) -> onTouchPad(buttons, x, y, 0, 0));
    }

    private void updateTouchVisibility() {
        if (touchControls != null)
            touchControls.setVisible(controlsEnabled && activityResumed && hasWindowFocus() && textDialog == null && !physicalController);
    }

    // 手柄是否接入。只认 SOURCE_GAMEPAD / SOURCE_JOYSTICK，避免把触屏、遥控器也算成手柄。
    // Is a gamepad connected: only SOURCE_GAMEPAD / SOURCE_JOYSTICK count, so touchscreens and remotes do not.
    private boolean anyGamepad() {
        InputManager manager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (manager == null) return false;
        for (int id : manager.getInputDeviceIds()) {
            InputDevice device = manager.getInputDevice(id);
            if (device == null) continue;
            int sources = device.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return true;
        }
        return false;
    }

    // 用设备级监听而不是事件分发：NativeActivity 把 AInputQueue 直接挂到主 Looper 消费了手柄事件，
    // 窗口的事件分发看不到它们；设备增删回调与谁消费事件无关，所以这条路可靠。
    // A device-level listener rather than event dispatch: NativeActivity attaches the AInputQueue to the main
    // Looper and consumes pad events, so window dispatch never sees them, while add/remove callbacks do not care
    // who consumes the events.
    private final InputManager.InputDeviceListener inputDeviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { refreshPhysicalController(); }
        @Override public void onInputDeviceRemoved(int id) { refreshPhysicalController(); }
        @Override public void onInputDeviceChanged(int id) { refreshPhysicalController(); }
    };

    private void refreshPhysicalController() {
        physicalController = anyGamepad();
        updateTouchVisibility();
    }

    @Override protected void onResume() {
        super.onResume();
        activityResumed = true;
        updateTouchVisibility();
    }

    @Override protected void onPause() {
        activityResumed = false;
        updateTouchVisibility();
        super.onPause();
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        updateTouchVisibility();
    }

    public void updateDebug(String text) {
        runOnUiThread(() -> { if (debugView != null && !isFinishing() && !isDestroyed()) debugView.setText(text); });
    }

    // 由原生调试线程在启动时调用一次；关掉时把浮层内容整个隐藏，而不只是停止刷新。
    // 弹窗本身留着不销毁（它是 setTouchable(false)，不抢输入），只是没有可见内容。
    // Called once at startup by the native debug thread. Hiding removes the overlay content entirely rather than
    // merely stopping refreshes; the popup itself stays (it is setTouchable(false) and takes no input), just empty.
    public void setDebugOverlay(boolean visible) {
        runOnUiThread(() -> {
            if (debugView != null && !isFinishing() && !isDestroyed())
                debugView.setVisibility(visible ? android.view.View.VISIBLE : android.view.View.GONE);
        });
    }

    private void finishText(long request, String text, boolean accepted) {
        if (textRequest != request) return;
        textRequest = 0;
        onTextResult(request, text, accepted);
        if (textDialog != null) { textDialog.dismiss(); textDialog = null; }
        updateTouchVisibility();
    }

    public void requestText(long request, String title, String message, String hint,
                            String initial, int maxChars, int numericMax, boolean nameRule) {
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) { onTextResult(request, "", false); return; }
            if (textDialog != null) finishText(textRequest, "", false);
            textRequest = request;
            EditText input = new EditText(this);
            input.setSingleLine(true);
            input.setInputType(numericMax >= 0 ? InputType.TYPE_CLASS_NUMBER :
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maxChars > 0 ? maxChars : 1002)});
            input.setHint(hint);
            input.setText(initial);
            input.setSelection(input.length());
            int margin = (int)(24 * getResources().getDisplayMetrics().density);
            input.setPadding(margin, margin / 2, margin, margin / 2);
            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title).setMessage(message).setView(input)
                .setPositiveButton(android.R.string.ok, null)
                .setNegativeButton(android.R.string.cancel, (d, which) -> finishText(request, "", false))
                .create();
            textDialog = dialog;
            updateTouchVisibility();
            dialog.setOnCancelListener(d -> finishText(request, "", false));
            dialog.setOnDismissListener(d -> finishText(request, "", false));
            dialog.setOnShowListener(d -> {
                Runnable validate = () -> {
                    String text = input.getText().toString();
                    boolean valid = true;
                    if (nameRule && !text.isEmpty()) {
                        int bytes = text.getBytes(StandardCharsets.UTF_8).length;
                        int separators = 0;
                        for (int i = 0; i < text.length(); i++) {
                            char c = text.charAt(i);
                            if (c == '-' || c == '_') separators++;
                            else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '\u4e00' && c <= '\u9fff') || (c >= '\u3400' && c <= '\u4dbf'))) valid = false;
                        }
                        valid &= bytes >= 2 && bytes <= 15 && separators <= 1 &&
                            text.charAt(0) != '-' && text.charAt(0) != '_' &&
                            text.charAt(text.length() - 1) != '-' && text.charAt(text.length() - 1) != '_';
                    }
                    if (numericMax >= 0) {
                        try { long value = Long.parseLong(text); valid = !text.isEmpty() && value >= 0 && value <= numericMax; }
                        catch (NumberFormatException e) { valid = false; }
                    }
                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(valid);
                };
                input.addTextChangedListener(new TextWatcher() {
                    public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                    public void onTextChanged(CharSequence s, int start, int before, int count) { validate.run(); }
                    public void afterTextChanged(Editable s) {}
                });
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v ->
                    finishText(request, input.getText().toString(), true));
                validate.run();
                input.requestFocus();
                dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE |
                    WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            });
            dialog.show();
        });
    }

    @Override protected void onDestroy() {
        hostRunning = false;
        if (batteryReceiver != null) { unregisterReceiver(batteryReceiver); batteryReceiver = null; }
        InputManager inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(inputDeviceListener);
        if (touchControls != null) touchControls.destroy();
        if (debugPopup != null) debugPopup.dismiss();
        if (textDialog != null) finishText(textRequest, "", false);
        super.onDestroy();
    }
}
