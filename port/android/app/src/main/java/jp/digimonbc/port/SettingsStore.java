package jp.digimonbc.port;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The Android launcher edits the same settings.ini the native player reads.
 *
 * <p>The file lives in {@code files/settings.ini}, which is exactly the folder SDL's
 * preference path resolves to on Android. Unknown keys (controller bindings, for
 * example) are preserved verbatim so the desktop launcher and the in-game menu keep
 * working.</p>
 */
final class SettingsStore {
    private final File file;
    private final LinkedHashMap<String, String> values = new LinkedHashMap<>();

    SettingsStore(Context context) {
        file = new File(context.getFilesDir(), "settings.ini");
        load();
    }

    File file() {
        return file;
    }

    private void load() {
        if (!file.isFile()) return;
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                String key = line.substring(0, eq).trim();
                if (key.isEmpty() || key.startsWith("#") || key.startsWith(";")) continue;
                values.put(key, line.substring(eq + 1).trim());
            }
        } catch (IOException ignored) {
            // Defaults are used; a later save recreates the file.
        }
    }

    String get(String key, String fallback) {
        String value = values.get(key);
        return value == null ? fallback : value;
    }

    int getInt(String key, int fallback) {
        try {
            return Integer.parseInt(get(key, Integer.toString(fallback)));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    boolean getBool(String key, boolean fallback) {
        String value = values.get(key);
        if (value == null) return fallback;
        return !value.equals("0") && !value.equalsIgnoreCase("false");
    }

    void put(String key, String value) {
        values.put(key, value);
    }

    void put(String key, int value) {
        values.put(key, Integer.toString(value));
    }

    void put(String key, boolean value) {
        values.put(key, value ? "1" : "0");
    }

    void clear() {
        values.clear();
    }

    void save() throws IOException {
        File dir = file.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs())
            throw new IOException("cannot create " + dir);

        File tmp = new File(dir, "settings.ini.tmp");
        try (Writer writer = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            writer.write("# Digimon Battle Chronicle settings. Delete this file to restore defaults.\n");
            for (Map.Entry<String, String> entry : values.entrySet())
                writer.write(entry.getKey() + "=" + entry.getValue() + "\n");
        }
        try {
            Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException e) {
            if (!tmp.renameTo(file)) {
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
                throw e;
            }
        }
    }
}
