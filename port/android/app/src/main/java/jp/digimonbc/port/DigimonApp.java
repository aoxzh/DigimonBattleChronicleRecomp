package jp.digimonbc.port;

import android.app.Application;

import com.google.android.material.color.DynamicColors;

public final class DigimonApp extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        // Material You: use the wallpaper palette on Android 12+, fall back to the
        // warm orange theme in values/colors.xml on older devices.
        DynamicColors.applyToActivitiesIfAvailable(this);
    }
}
