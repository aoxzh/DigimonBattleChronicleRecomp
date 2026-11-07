package jp.digimonbc.port;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.util.Properties;

/**
 * SDL surface for the recompiled game. The disc is imported and validated by
 * MainActivity first, so this activity only has to hand the native player a
 * normal absolute path.
 */
public class GameActivity extends SDLActivity {
    private static final String PREFS = "launcher";
    private static final String KEY_DISC_PATH = "disc_path";

    private String discPath;

    @Override
    protected String[] getLibraries() {
        return new String[]{"c++_shared", "SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        discPath = getIntent().getStringExtra("disc_path");
        if (discPath == null)
            discPath = getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_DISC_PATH, null);
        if (discPath == null || !new File(discPath).canRead()) {
            startActivity(new Intent(this, MainActivity.class));
            finish();
            return;
        }

        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
    }

    /** argv for SDL_main. */
    @Override
    protected String[] getArguments() {
        if (discPath == null) return new String[0];
        // ADB/run-as can place the same frame-based replay used by desktop tests
        // in private app storage. Removing it restores ordinary player input.
        File replay = new File(getFilesDir(), "input-script.json");
        if (replay.isFile()) {
            // Optional native frame dumps for an automated replay. Kept in app-private
            // storage so a device test can capture consecutive frames without ADB
            // screenshot traffic changing the cadence being investigated.
            File options = new File(getFilesDir(), "replay.properties");
            if (options.isFile()) {
                try (FileInputStream input = new FileInputStream(options)) {
                    Properties properties = new Properties();
                    properties.load(input);
                    for (String key : new String[]{"GC_DUMP_DIR", "GC_DUMP_EVERY", "GCN_DUMP_RANGE", "GC_AUDIO_TRACE",
                            "GCN_STALLS", "GCN_FRAMETIME", "GC_THREAD_DIAG", "GCN_XF_THREADS", "GC_FE_THREAD", "GC_COPY_WB", "GC_GUEST_CPUS", "GCN_XF_JOBS"}) {
                        String value = properties.getProperty(key);
                        if (value != null) Os.setenv(key, value, true);
                    }
                } catch (Exception error) {
                    Log.e("DigimonReplay", "Cannot configure replay capture", error);
                }
            }
            return new String[]{"--disc", discPath, "--input-script", replay.getAbsolutePath()};
        }
        return new String[]{"--disc", discPath};
    }
}
