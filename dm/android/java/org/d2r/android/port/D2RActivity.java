// 原生游戏窗口与 Android 文本输入；游戏线程等待结果，主线程保持响应。
// Native game window and Android text entry; the guest waits for a result while the UI remains responsive.
package org.d2r.android.port;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.TextWatcher;
import android.text.InputType;
import android.view.WindowManager;
import android.view.Display;
import android.util.Log;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
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
    private static native void onTextResult(long request, String text, boolean accepted);
    private static native void onTouchPad(long buttons, int leftX, int leftY, int rightX, int rightY);

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
        touchControls = new TouchControls(this, (buttons, x, y) -> onTouchPad(buttons, x, y, 0, 0));
    }

    private void updateTouchVisibility() {
        if (touchControls != null) touchControls.setVisible(activityResumed && hasWindowFocus() && textDialog == null);
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
        if (batteryReceiver != null) { unregisterReceiver(batteryReceiver); batteryReceiver = null; }
        if (touchControls != null) touchControls.destroy();
        if (debugPopup != null) debugPopup.dismiss();
        if (textDialog != null) finishText(textRequest, "", false);
        super.onDestroy();
    }
}
