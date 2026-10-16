// 原生游戏窗口与 Android 文本输入；游戏线程等待结果，主线程保持响应。
// Native game window and Android text entry; the guest waits for a result while the UI remains responsive.
package org.d2r.android.port;

import android.app.AlertDialog;
import android.app.NativeActivity;
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
    private static native void onTextResult(long request, String text, boolean accepted);
    private static native void onTouchPad(long buttons, int leftX, int leftY, int rightX, int rightY);

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
        if (touchControls != null) touchControls.destroy();
        if (debugPopup != null) debugPopup.dismiss();
        if (textDialog != null) finishText(textRequest, "", false);
        super.onDestroy();
    }
}
