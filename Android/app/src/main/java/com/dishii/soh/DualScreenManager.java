package com.dishii.soh;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.util.Log;
import android.view.Display;

/**
 * Detects physically separate secondary displays (e.g. the second screen of a Surface Duo or an
 * LG dual-screen case) and shows a {@link DualScreenHudPresentation} on the first one found, so the
 * native Dual Screen HUD feature can redirect the core in-game HUD onto it.
 *
 * Single-screen devices never have a display with {@link Display#FLAG_PRESENTATION}, so this class
 * is a no-op for them - nothing is shown and native code always reports no secondary display.
 */
public class DualScreenManager {
    private static final String TAG = "DualScreenManager";

    private final MainActivity activity;
    private final DisplayManager displayManager;
    private DualScreenHudPresentation presentation;

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override
        public void onDisplayAdded(int displayId) {
            Display display = displayManager.getDisplay(displayId);
            if (isPresentationDisplay(display)) {
                showPresentation(display);
            }
        }

        @Override
        public void onDisplayRemoved(int displayId) {
            if (presentation != null && presentation.getDisplay() != null
                    && presentation.getDisplay().getDisplayId() == displayId) {
                dismissPresentation();
            }
        }

        @Override
        public void onDisplayChanged(int displayId) {
            // No-op: resolution/orientation changes are handled by the SurfaceView's own callbacks.
        }
    };

    public DualScreenManager(MainActivity activity) {
        this.activity = activity;
        this.displayManager = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
    }

    private static boolean isPresentationDisplay(Display display) {
        return display != null && (display.getFlags() & Display.FLAG_PRESENTATION) != 0;
    }

    public void start() {
        if (displayManager == null) {
            return;
        }

        displayManager.registerDisplayListener(displayListener, null);

        for (Display display : displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (isPresentationDisplay(display)) {
                showPresentation(display);
                break;
            }
        }
    }

    public void stop() {
        if (displayManager != null) {
            displayManager.unregisterDisplayListener(displayListener);
        }
        dismissPresentation();
    }

    private void showPresentation(Display display) {
        dismissPresentation();
        try {
            presentation = new DualScreenHudPresentation(activity, display);
            presentation.show();
        } catch (Exception e) {
            Log.w(TAG, "Failed to show Dual Screen HUD presentation", e);
            presentation = null;
        }
    }

    private void dismissPresentation() {
        if (presentation != null) {
            presentation.dismiss();
            presentation = null;
        }
    }
}
