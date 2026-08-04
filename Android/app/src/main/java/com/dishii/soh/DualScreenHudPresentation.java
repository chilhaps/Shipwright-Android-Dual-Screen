package com.dishii.soh;

import android.app.Presentation;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;

/**
 * Hosts a full-screen {@link SurfaceView} on a secondary physical display and forwards its Surface
 * lifecycle to native code, which redirects the core in-game HUD onto it. See
 * soh/soh/Enhancements/DualScreenHUD/DualScreenHUD.cpp for the native side.
 */
public class DualScreenHudPresentation extends Presentation {
    private static final String TAG = "DualScreenHudPresentation";

    private final MainActivity activity;

    private final SurfaceHolder.Callback surfaceCallback = new SurfaceHolder.Callback() {
        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            // surfaceChanged() is always called right after surfaceCreated() with the actual size.
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            activity.nativeSecondaryDisplaySurfaceChanged(holder.getSurface(), width, height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            activity.nativeSecondaryDisplaySurfaceDestroyed();
        }
    };

    public DualScreenHudPresentation(MainActivity activity, Display display) {
        super(activity, display);
        this.activity = activity;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SurfaceView surfaceView = new SurfaceView(getContext());
        surfaceView.getHolder().addCallback(surfaceCallback);
        setContentView(surfaceView, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    public void dismiss() {
        try {
            super.dismiss();
        } catch (Exception e) {
            Log.w(TAG, "Error dismissing Dual Screen HUD presentation", e);
        }
    }
}
