package jp.digimonbc.port;

import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.documentfile.provider.DocumentFile;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.color.MaterialColors;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.progressindicator.LinearProgressIndicator;
import com.google.android.material.slider.Slider;
import com.google.android.material.textfield.MaterialAutoCompleteTextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Material 3 startup screen for Android.
 *
 * <p>The disc is imported through the system document picker and copied into the
 * app's internal files directory. Native code then receives a normal absolute
 * path, avoiding the /proc/self/fd content-URI workaround that cannot be reopened
 * reliably on current Android storage providers.</p>
 */
public final class MainActivity extends AppCompatActivity {
    private static final String TAG = "DigimonBC";
    private static final String PREFS = "launcher";
    private static final String KEY_DISC_PATH = "disc_path";
    private static final String KEY_DISC_NAME = "disc_name";
    private static final String KEY_SOURCE_URI = "disc_source_uri";
    private static final String KEY_SOURCE_NAME = "disc_source_name";
    private static final String KEY_TREE_URI = "download_tree_uri";
    private static final String DISC_FILE = "disc.iso";
    private static final int[] LATENCIES = {0, 50, 80, 120, 200, 350};

    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final AtomicBoolean destroyed = new AtomicBoolean();

    private SettingsStore settings;
    private SharedPreferences prefs;

    private View content;
    private View scroll;
    private MaterialCardView bottomBar;
    private TextView discName;
    private TextView discMeta;
    private TextView discChip;
    private TextView discError;
    private TextView discCopyDetail;
    private TextView discRemembered;
    private TextView volumeValue;
    private TextView opacityValue;
    private TextView sizeValue;
    private LinearProgressIndicator discProgress;
    private MaterialButton pickDisc;
    private MaterialButton scanDownloads;
    private MaterialButton play;
    private MaterialButtonToggleGroup resolutionGroup;
    private MaterialButtonToggleGroup aspectGroup;
    private MaterialSwitch switchSmooth;
    private MaterialSwitch switchFps;
    private MaterialSwitch switchSound;
    private MaterialSwitch switchHaptics;
    private MaterialSwitch switchMemcard;
    private Slider volumeSlider;
    private Slider opacitySlider;
    private Slider sizeSlider;
    private MaterialAutoCompleteTextView latency;
    private MaterialAutoCompleteTextView touchMode;

    private String discPath;
    private String discNameText;
    private boolean discValid;
    private boolean importing;

    private final ActivityResultLauncher<String[]> pickDocument =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), this::onDiscPicked);
    private final ActivityResultLauncher<Uri> openTree =
            registerForActivityResult(new ActivityResultContracts.OpenDocumentTree(), this::onTreePicked);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        setContentView(R.layout.activity_main);

        settings = new SettingsStore(this);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        bindViews();
        applyLauncherArt();
        applyInsets();
        setupDropdowns();
        setupListeners();
        applySettingsToUi();
        restoreDisc();
    }

    /** Lets the background art show through the settings cards (a frosted, not opaque, surface). */
    private void applyLauncherArt() {
        android.view.ViewGroup group = (android.view.ViewGroup) content;
        for (int i = 0; i < group.getChildCount(); i++) {
            View child = group.getChildAt(i);
            if (child instanceof MaterialCardView) {
                MaterialCardView card = (MaterialCardView) child;
                ColorStateList bg = card.getCardBackgroundColor();
                card.setCardBackgroundColor(bg.withAlpha(214));
            }
        }
        MaterialCardView bar = findViewById(R.id.bottom_bar);
        bar.setCardBackgroundColor(bar.getCardBackgroundColor().withAlpha(232));
    }

    private void bindViews() {
        content = findViewById(R.id.content);
        scroll = findViewById(R.id.scroll);
        bottomBar = findViewById(R.id.bottom_bar);
        discName = findViewById(R.id.disc_name);
        discMeta = findViewById(R.id.disc_meta);
        discChip = findViewById(R.id.disc_chip);
        discError = findViewById(R.id.disc_error);
        discCopyDetail = findViewById(R.id.disc_copy_detail);
        discRemembered = findViewById(R.id.disc_remembered);
        volumeValue = findViewById(R.id.volume_value);
        opacityValue = findViewById(R.id.opacity_value);
        sizeValue = findViewById(R.id.size_value);
        discProgress = findViewById(R.id.disc_progress);
        pickDisc = findViewById(R.id.pick_disc);
        scanDownloads = findViewById(R.id.scan_downloads);
        play = findViewById(R.id.play);
        resolutionGroup = findViewById(R.id.resolution_group);
        aspectGroup = findViewById(R.id.aspect_group);
        switchSmooth = findViewById(R.id.switch_smooth);
        switchFps = findViewById(R.id.switch_fps);
        switchSound = findViewById(R.id.switch_sound);
        switchHaptics = findViewById(R.id.switch_haptics);
        switchMemcard = findViewById(R.id.switch_memcard);
        volumeSlider = findViewById(R.id.volume_slider);
        opacitySlider = findViewById(R.id.opacity_slider);
        sizeSlider = findViewById(R.id.size_slider);
        latency = findViewById(R.id.latency);
        touchMode = findViewById(R.id.touch_mode);

        TextView version = findViewById(R.id.version);
        version.setText(getString(R.string.version, BuildConfig.VERSION_NAME));
    }

    private void applyInsets() {
        final int contentLeft = content.getPaddingLeft();
        final int contentTop = content.getPaddingTop();
        final int contentRight = content.getPaddingRight();
        final int bottomPadding = bottomBar.getPaddingBottom();

        View root = findViewById(R.id.root);
        ViewCompat.setOnApplyWindowInsetsListener(root, (view, windowInsets) -> {
            Insets bars = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            content.setPadding(contentLeft, contentTop + bars.top, contentRight, content.getPaddingBottom());
            bottomBar.setPadding(0, 0, 0, bottomPadding + bars.bottom);
            scroll.setPadding(0, 0, 0, bars.bottom);

            boolean light = (getResources().getConfiguration().uiMode
                    & Configuration.UI_MODE_NIGHT_MASK) != Configuration.UI_MODE_NIGHT_YES;
            WindowInsetsControllerCompat controller =
                    WindowCompat.getInsetsController(getWindow(), view);
            // The status bar sits on the dark launcher art: always light icons there.
            controller.setAppearanceLightStatusBars(false);
            controller.setAppearanceLightNavigationBars(light);
            return windowInsets;
        });
        ViewCompat.requestApplyInsets(root);
    }

    private void setupDropdowns() {
        String[] latencyLabels = {
                getString(R.string.latency_auto), getString(R.string.latency_50),
                getString(R.string.latency_80), getString(R.string.latency_120),
                getString(R.string.latency_200), getString(R.string.latency_350)};
        latency.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, latencyLabels));
        latency.setOnItemClickListener((parent, view, position, id) -> {
            settings.put("audio_latency", LATENCIES[position]);
        });

        String[] touchLabels = {
                getString(R.string.touch_mode_auto), getString(R.string.touch_mode_always),
                getString(R.string.touch_mode_never)};
        touchMode.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, touchLabels));
        touchMode.setOnItemClickListener((parent, view, position, id) ->
                settings.put("touch_mode", position));
    }

    private void setupListeners() {
        pickDisc.setOnClickListener(view -> {
            if (importing) return;
            try {
                pickDocument.launch(new String[]{"*/*"});
            } catch (ActivityNotFoundException e) {
                showError(getString(R.string.pick_failed));
            }
        });

        scanDownloads.setOnClickListener(view -> onScanDownloads());
        play.setOnClickListener(view -> launchGame());

        findViewById(R.id.about_button).setOnClickListener(view -> showAbout());
        findViewById(R.id.reset_settings).setOnClickListener(view -> confirmReset());

        volumeSlider.addOnChangeListener((slider, value, fromUser) -> {
            volumeValue.setText(String.format(Locale.getDefault(), "%d%%", Math.round(value)));
            if (fromUser) switchSound.setChecked(true);
        });
        opacitySlider.addOnChangeListener((slider, value, fromUser) ->
                opacityValue.setText(String.format(Locale.getDefault(), "%d%%", Math.round(value))));
        sizeSlider.addOnChangeListener((slider, value, fromUser) ->
                sizeValue.setText(String.format(Locale.getDefault(), "%d%%", Math.round(value))));

        switchSound.setOnCheckedChangeListener((button, checked) ->
                setVolumeEnabled(checked));
    }

    private void setVolumeEnabled(boolean enabled) {
        volumeSlider.setEnabled(enabled);
        volumeSlider.setAlpha(enabled ? 1.0f : 0.38f);
        volumeValue.setAlpha(enabled ? 1.0f : 0.38f);
    }

    private void applySettingsToUi() {
        int scale = Math.max(1, Math.min(4, settings.getInt("render_scale", 2)));
        int[] scaleIds = {R.id.res_1, R.id.res_2, R.id.res_3, R.id.res_4};
        resolutionGroup.check(scaleIds[scale - 1]);

        boolean stretch = settings.getBool("stretch", false);
        aspectGroup.check(stretch ? R.id.aspect_stretch : R.id.aspect_43);

        switchSmooth.setChecked(settings.getBool("smooth", true));
        switchFps.setChecked(settings.getBool("show_fps", false));
        boolean audio = settings.getBool("audio", true);
        switchSound.setChecked(audio);
        setVolumeEnabled(audio);
        switchHaptics.setChecked(settings.getBool("touch_haptics", true));
        switchMemcard.setChecked(settings.getBool("memcard", true));

        int volume = Math.max(0, Math.min(100, settings.getInt("volume", 100)));
        volumeSlider.setValue(volume);
        volumeValue.setText(String.format(Locale.getDefault(), "%d%%", volume));

        int opacity = Math.max(10, Math.min(100, settings.getInt("touch_opacity", 55)));
        opacitySlider.setValue(opacity);
        opacityValue.setText(String.format(Locale.getDefault(), "%d%%", opacity));

        int size = Math.max(50, Math.min(200, settings.getInt("touch_size", 100)));
        sizeSlider.setValue(size);
        sizeValue.setText(String.format(Locale.getDefault(), "%d%%", size));

        int latencyValue = settings.getInt("audio_latency", 0);
        int latencyIndex = 0;
        for (int i = 0; i < LATENCIES.length; i++)
            if (LATENCIES[i] == latencyValue) latencyIndex = i;
        latency.setText(getResources().getStringArray(R.array.latency_labels)[latencyIndex], false);

        int mode = Math.max(0, Math.min(2, settings.getInt("touch_mode", 0)));
        touchMode.setText(getResources().getStringArray(R.array.touch_mode_labels)[mode], false);
    }

    private void saveUiToSettings() {
        if (settings == null) return;
        int checked = resolutionGroup.getCheckedButtonId();
        int scale = checked == R.id.res_1 ? 1 : checked == R.id.res_3 ? 3 : checked == R.id.res_4 ? 4 : 2;
        settings.put("render_scale", scale);
        settings.put("stretch", aspectGroup.getCheckedButtonId() == R.id.aspect_stretch);
        settings.put("smooth", switchSmooth.isChecked());
        settings.put("show_fps", switchFps.isChecked());
        settings.put("audio", switchSound.isChecked());
        settings.put("volume", Math.round(volumeSlider.getValue()));
        settings.put("audio_latency", LATENCIES[latencyIndex()]);
        settings.put("touch_mode", touchModeIndex());
        settings.put("touch_opacity", Math.round(opacitySlider.getValue()));
        settings.put("touch_size", Math.round(sizeSlider.getValue()));
        settings.put("touch_haptics", switchHaptics.isChecked());
        settings.put("memcard", switchMemcard.isChecked());
        settings.put("fullscreen", 1);
        settings.put("skip_launcher", 0);
        settings.put("disc", discPath == null ? "" : discPath);
        try {
            settings.save();
        } catch (IOException e) {
            Toast.makeText(this, getString(R.string.copy_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    private int latencyIndex() {
        CharSequence text = latency.getText();
        String[] labels = getResources().getStringArray(R.array.latency_labels);
        for (int i = 0; i < labels.length; i++) if (labels[i].contentEquals(text)) return i;
        int value = settings.getInt("audio_latency", 0);
        for (int i = 0; i < LATENCIES.length; i++) if (LATENCIES[i] == value) return i;
        return 0;
    }

    private int touchModeIndex() {
        CharSequence text = touchMode.getText();
        String[] labels = getResources().getStringArray(R.array.touch_mode_labels);
        for (int i = 0; i < labels.length; i++) if (labels[i].contentEquals(text)) return i;
        return Math.max(0, Math.min(2, settings.getInt("touch_mode", 0)));
    }

    // ---- Disc selection -------------------------------------------------

    private void restoreDisc() {
        String savedPath = prefs.getString(KEY_DISC_PATH, null);
        String savedName = prefs.getString(KEY_DISC_NAME, null);
        List<String> candidates = new ArrayList<>();
        if (savedPath != null) candidates.add(savedPath);
        String iniDisc = settings.get("disc", "");
        if (!iniDisc.isEmpty()) candidates.add(iniDisc);
        File internal = new File(getFilesDir(), DISC_FILE);
        candidates.add(internal.getAbsolutePath());
        File external = getExternalFilesDir(null) == null ? null : new File(getExternalFilesDir(null), DISC_FILE);
        if (external != null) candidates.add(external.getAbsolutePath());

        Set<String> seen = new LinkedHashSet<>();
        for (String candidate : candidates) {
            if (candidate == null) continue;
            if (candidate.startsWith("/proc/")) continue;
            if (!seen.add(candidate)) continue;
            File file = new File(candidate);
            if (!file.isFile() || !file.canRead() || file.length() < 0x440) continue;
            discPath = file.getAbsolutePath();
            if (savedPath != null && savedPath.equals(discPath) && !TextUtils.isEmpty(savedName))
                discNameText = savedName;
            else if (DISC_FILE.equals(file.getName()))
                discNameText = getString(R.string.disc_default_name);
            else
                discNameText = file.getName();
            validateCurrentDisc();
            return;
        }
        showDiscNone();
        tryRestoreFromSource();
    }

    /** If the remembered source is still readable, re-import it automatically. */
    private void tryRestoreFromSource() {
        String source = prefs.getString(KEY_SOURCE_URI, null);
        if (source == null) return;
        Uri uri = Uri.parse(source);
        if (!hasPersistedRead(uri)) {
            discError.setText(R.string.restore_failed);
            discError.setVisibility(View.VISIBLE);
            setChip(R.string.chip_error,
                    com.google.android.material.R.attr.colorErrorContainer,
                    com.google.android.material.R.attr.colorOnErrorContainer);
            return;
        }
        String name = prefs.getString(KEY_SOURCE_NAME, null);
        if (TextUtils.isEmpty(name)) name = uri.getLastPathSegment();
        startImport(uri, TextUtils.isEmpty(name) ? getString(R.string.disc_default_name) : name, 0);
    }

    private boolean hasPersistedRead(Uri uri) {
        for (UriPermission permission : getContentResolver().getPersistedUriPermissions()) {
            if (!permission.isReadPermission()) continue;
            Uri granted = permission.getUri();
            if (granted.equals(uri)) return true;
            // A persisted tree grant also covers its child documents.
            String tree = granted.toString();
            String child = uri.toString();
            if (child.startsWith(tree) && child.length() > tree.length()) {
                char next = child.charAt(tree.length());
                if (next == '/' || next == '?') return true;
            }
        }
        return false;
    }

    private void validateCurrentDisc() {
        if (discPath == null) {
            showDiscNone();
            return;
        }
        importing = false;
        discValid = false;
        setChip(R.string.chip_checking,
                com.google.android.material.R.attr.colorSecondaryContainer,
                com.google.android.material.R.attr.colorOnSecondaryContainer);
        discName.setText(discNameText == null ? getString(R.string.disc_checking) : discNameText);
        discMeta.setText(R.string.disc_checking);
        discError.setVisibility(View.GONE);
        discRemembered.setVisibility(View.GONE);
        discProgress.setVisibility(View.GONE);
        discCopyDetail.setVisibility(View.GONE);
        pickDisc.setEnabled(false);
        play.setText(R.string.play);
        play.setEnabled(false);

        final String path = discPath;
        io.execute(() -> {
            DiscImage.Result result = DiscImage.check(new File(path));
            runOnUiThread(() -> {
                if (destroyed.get() || !path.equals(discPath)) return;
                pickDisc.setEnabled(true);
                if (result.ok()) {
                    discValid = true;
                    showDiscReady(result);
                } else {
                    discValid = false;
                    showDiscError(describe(result), result);
                }
            });
        });
    }

    private void onDiscPicked(Uri uri) {
        Log.i(TAG, "onDiscPicked " + uri);
        if (uri == null) return;
        ContentResolver resolver = getContentResolver();
        String name = queryName(resolver, uri);
        long size = querySize(resolver, uri);
        if (TextUtils.isEmpty(name)) name = uri.getLastPathSegment();
        if (TextUtils.isEmpty(name)) name = DISC_FILE;
        name = Uri.decode(name);

        String ext = DiscImage.extension(name);
        if (!ext.equals(".iso") && !ext.equals(".gcm")) {
            showError(getString(R.string.invalid_ext));
            return;
        }
        try {
            resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // The current grant still allows this import; future re-import may ask again.
        }
        startImport(uri, name, size);
    }

    // ---- Download folder scan (SAF tree, no broad storage permission) -----

    private void onScanDownloads() {
        String saved = prefs.getString(KEY_TREE_URI, null);
        Log.i(TAG, "onScanDownloads importing=" + importing + " tree=" + saved);
        if (importing) return;
        if (saved != null) {
            scanTree(Uri.parse(saved));
            return;
        }
        new MaterialAlertDialogBuilder(this)
                .setTitle(R.string.disc_scan)
                .setMessage(R.string.scan_intro)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.continue_label, (dialog, which) -> launchTreePicker())
                .show();
    }

    private void launchTreePicker() {
        try {
            // DocumentsUI opens at Download; the user only has to confirm the folder.
            Uri initial = DocumentsContract.buildDocumentUri(
                    "com.android.externalstorage.documents", "primary:Download");
            openTree.launch(initial);
        } catch (Exception e) {
            openTree.launch(null);
        }
    }

    private void onTreePicked(Uri treeUri) {
        Log.i(TAG, "onTreePicked " + treeUri);
        if (treeUri == null) return;
        try {
            getContentResolver().takePersistableUriPermission(
                    treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            showError(getString(R.string.scan_permission_failed));
            return;
        }
        prefs.edit().putString(KEY_TREE_URI, treeUri.toString()).apply();
        scanTree(treeUri);
    }

    private void scanTree(Uri treeUri) {
        if (importing) return;
        scanDownloads.setEnabled(false);
        io.execute(() -> {
            List<DocumentFile> found = new ArrayList<>();
            String error = null;
            try {
                DocumentFile root = DocumentFile.fromTreeUri(this, treeUri);
                if (root == null || !root.canRead()) {
                    error = getString(R.string.scan_permission_failed);
                } else {
                    collectImages(root, found, 0, new int[]{0});
                }
            } catch (Exception e) {
                error = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
            }
            found.sort((a, b) -> String.CASE_INSENSITIVE_ORDER.compare(
                    String.valueOf(a.getName()), String.valueOf(b.getName())));
            final String failure = error;
            runOnUiThread(() -> {
                if (destroyed.get()) return;
                scanDownloads.setEnabled(true);
                if (failure != null) {
                    prefs.edit().remove(KEY_TREE_URI).apply();
                    showError(getString(R.string.scan_tree_error, failure));
                    return;
                }
                showCandidates(found);
            });
        });
    }

    private static void collectImages(DocumentFile dir, List<DocumentFile> out, int depth, int[] visited) {
        if (dir == null || depth > 2 || visited[0] > 2000) return;
        DocumentFile[] children = dir.listFiles();
        for (DocumentFile file : children) {
            if (++visited[0] > 2000) return;
            String name = file.getName();
            if (name == null || name.startsWith(".")) continue;
            if (file.isDirectory()) {
                collectImages(file, out, depth + 1, visited);
                continue;
            }
            if (!file.isFile()) continue;
            String ext = DiscImage.extension(name);
            if ((ext.equals(".iso") || ext.equals(".gcm")) && file.length() >= 0x440)
                out.add(file);
        }
    }

    private void showCandidates(List<DocumentFile> files) {
        Log.i(TAG, "showCandidates " + files.size());
        if (files.isEmpty()) {
            showError(getString(R.string.scan_none));
            return;
        }
        if (files.size() == 1) {
            confirmImport(files.get(0));
            return;
        }
        String[] labels = new String[files.size()];
        for (int i = 0; i < files.size(); i++) {
            DocumentFile file = files.get(i);
            labels[i] = getString(R.string.disc_file_meta, file.getName(),
                    formatBytes(file.length()));
        }
        new MaterialAlertDialogBuilder(this)
                .setTitle(R.string.scan_choose_title)
                .setItems(labels, (dialog, which) -> confirmImport(files.get(which)))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void confirmImport(DocumentFile file) {
        String name = file.getName();
        if (TextUtils.isEmpty(name)) name = getString(R.string.disc_default_name);
        long size = file.length();
        String finalName = name;
        new MaterialAlertDialogBuilder(this)
                .setTitle(R.string.scan_import_title)
                .setMessage(getString(R.string.scan_import_message, name, formatBytes(size)))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.import_label,
                        (dialog, which) -> startImport(file.getUri(), finalName, size))
                .show();
    }

    private void startImport(Uri uri, String name, long size) {
        File target = new File(getFilesDir(), DISC_FILE);
        File temp = new File(getFilesDir(), DISC_FILE + ".part");
        long required = size > 0 ? size + (64L << 20) : 0;
        if (required > 0) {
            StatFs stat = new StatFs(getFilesDir().getAbsolutePath());
            long available = stat.getAvailableBytes() + (target.isFile() ? target.length() : 0);
            if (available < required) {
                showError(getString(R.string.not_enough_space, formatBytes(required), formatBytes(available)));
                return;
            }
        }

        importing = true;
        discValid = false;
        prefs.edit().putString(KEY_SOURCE_URI, uri.toString())
                .putString(KEY_SOURCE_NAME, name).apply();
        discError.setVisibility(View.GONE);
        discRemembered.setVisibility(View.GONE);
        scanDownloads.setEnabled(false);
        discProgress.setVisibility(View.VISIBLE);
        discProgress.setIndeterminate(size <= 0);
        discProgress.setProgressCompat(0, false);
        discCopyDetail.setVisibility(size > 0 ? View.VISIBLE : View.GONE);
        discCopyDetail.setText(size > 0 ? getString(R.string.disc_copy_detail, formatBytes(0), formatBytes(size)) : "");
        discName.setText(name);
        discMeta.setText(R.string.disc_checking);
        setChip(R.string.chip_checking,
                com.google.android.material.R.attr.colorSecondaryContainer,
                com.google.android.material.R.attr.colorOnSecondaryContainer);
        pickDisc.setEnabled(false);
        play.setText(getString(R.string.status_copying, 0));
        play.setEnabled(false);

        io.execute(() -> {
            String failure = null;
            DiscImage.Result result = null;
            try (InputStream in = getContentResolver().openInputStream(uri);
                 FileOutputStream out = new FileOutputStream(temp)) {
                if (in == null) throw new IOException("openInputStream returned null");
                byte[] buffer = new byte[1 << 20];
                long done = 0;
                int lastPercent = -1;
                long lastUpdate = 0;
                int read;
                while ((read = in.read(buffer)) > 0) {
                    if (destroyed.get()) throw new IOException("cancelled");
                    out.write(buffer, 0, read);
                    done += read;
                    if (size > 0) {
                        int percent = (int) Math.min(100, done * 100 / size);
                        long now = SystemClock.elapsedRealtime();
                        if (percent != lastPercent && (now - lastUpdate > 120 || percent == 100)) {
                            lastPercent = percent;
                            lastUpdate = now;
                            final int p = percent;
                            final long d = done;
                            runOnUiThread(() -> updateImportProgress(name, p, d, size));
                        }
                    }
                }
                out.getFD().sync();
                result = DiscImage.check(temp);
                if (result.ok()) {
                    Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
                }
            } catch (Exception e) {
                failure = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
            } finally {
                if (temp.isFile()) //noinspection ResultOfMethodCallIgnored
                    temp.delete();
            }

            final String error = failure;
            final DiscImage.Result checked = result;
            runOnUiThread(() -> {
                if (destroyed.get()) return;
                importing = false;
                pickDisc.setEnabled(true);
                scanDownloads.setEnabled(true);
                discProgress.setVisibility(View.GONE);
                discCopyDetail.setVisibility(View.GONE);
                if (error != null) {
                    if (discPath != null) {
                        showDiscReady(DiscImage.check(new File(discPath)));
                        showError(getString(R.string.copy_failed, error));
                    } else {
                        discValid = false;
                        discName.setText(R.string.disc_none);
                        discMeta.setText(R.string.disc_none_hint);
                        discChip.setText(R.string.chip_error);
                        setChip(R.string.chip_error,
                                com.google.android.material.R.attr.colorErrorContainer,
                                com.google.android.material.R.attr.colorOnErrorContainer);
                        discError.setText(getString(R.string.copy_failed, error));
                        discError.setVisibility(View.VISIBLE);
                        play.setText(R.string.play);
                        play.setEnabled(false);
                    }
                    return;
                }
                if (checked != null && checked.ok()) {
                    discPath = target.getAbsolutePath();
                    discNameText = name;
                    discValid = true;
                    prefs.edit().putString(KEY_DISC_PATH, discPath)
                            .putString(KEY_DISC_NAME, discNameText).apply();
                    settings.put("disc", discPath);
                    showDiscReady(checked);
                } else {
                    String reason = checked == null ? getString(R.string.disc_io) : describe(checked);
                    if (discPath != null && new File(discPath).isFile()) {
                        showDiscReady(DiscImage.check(new File(discPath)));
                    } else {
                        discValid = false;
                        showDiscNone();
                    }
                    showError(reason);
                }
            });
        });
    }

    private void updateImportProgress(String name, int percent, long done, long total) {
        if (destroyed.get()) return;
        discProgress.setIndeterminate(false);
        discProgress.setProgressCompat(percent, true);
        discCopyDetail.setText(getString(R.string.disc_copy_detail,
                formatBytes(done), formatBytes(total)));
        discName.setText(name);
        discMeta.setText(getString(R.string.disc_copying, percent));
        play.setText(getString(R.string.status_copying, percent));
    }

    // ---- Disc states ----------------------------------------------------

    private void showDiscNone() {
        discPath = null;
        discValid = false;
        importing = false;
        discNameText = null;
        discName.setText(R.string.disc_none);
        discMeta.setText(R.string.disc_none_hint);
        discError.setVisibility(View.GONE);
        discRemembered.setVisibility(View.GONE);
        discProgress.setVisibility(View.GONE);
        discCopyDetail.setVisibility(View.GONE);
        pickDisc.setText(R.string.disc_choose);
        pickDisc.setEnabled(true);
        scanDownloads.setEnabled(true);
        play.setText(R.string.play);
        play.setEnabled(false);
        setChip(R.string.chip_none,
                com.google.android.material.R.attr.colorSurfaceContainerHighest,
                com.google.android.material.R.attr.colorOnSurfaceVariant);
    }

    private void showDiscReady(DiscImage.Result result) {
        if (result == null || !result.ok()) {
            if (result != null) showDiscError(describe(result), result);
            return;
        }
        discValid = true;
        importing = false;
        discProgress.setVisibility(View.GONE);
        discCopyDetail.setVisibility(View.GONE);
        discError.setVisibility(View.GONE);
        discRemembered.setVisibility(View.VISIBLE);
        discName.setText(discNameText == null || discNameText.isEmpty()
                ? new File(discPath).getName() : discNameText);
        discMeta.setText(getString(R.string.disc_meta_ok, formatBytes(result.size)));
        pickDisc.setText(R.string.disc_change);
        pickDisc.setEnabled(true);
        play.setText(R.string.play);
        play.setEnabled(true);
        setChip(R.string.chip_ready,
                com.google.android.material.R.attr.colorTertiaryContainer,
                com.google.android.material.R.attr.colorOnTertiaryContainer);
    }

    private void showDiscError(String message, DiscImage.Result result) {
        discValid = false;
        discProgress.setVisibility(View.GONE);
        discCopyDetail.setVisibility(View.GONE);
        discRemembered.setVisibility(View.GONE);
        discError.setText(message);
        discError.setVisibility(View.VISIBLE);
        pickDisc.setText(R.string.disc_change);
        pickDisc.setEnabled(true);
        play.setText(R.string.play);
        play.setEnabled(false);
        if (result != null && result.size > 0)
            discMeta.setText(getString(R.string.disc_file_meta, formatBytes(result.size), message));
        else
            discMeta.setText(message);
        setChip(R.string.chip_error,
                com.google.android.material.R.attr.colorErrorContainer,
                com.google.android.material.R.attr.colorOnErrorContainer);
    }

    private void setChip(int text, int backgroundAttr, int foregroundAttr) {
        discChip.setText(text);
        int background = MaterialColors.getColor(discChip, backgroundAttr);
        int foreground = MaterialColors.getColor(discChip, foregroundAttr);
        ViewCompat.setBackgroundTintList(discChip, ColorStateList.valueOf(background));
        discChip.setTextColor(foreground);
    }

    private String describe(DiscImage.Result result) {
        switch (result.status) {
            case COMPRESSED:
                return getString(R.string.disc_compressed);
            case TOO_SMALL:
                return getString(R.string.disc_small);
            case NOT_GAMECUBE:
                return getString(R.string.disc_not_gc);
            case WRONG_GAME:
                return getString(R.string.disc_wrong_game, result.gameName, result.gameId);
            case WRONG_REV:
                return getString(R.string.disc_wrong_rev, result.revision);
            case DOL_MISMATCH:
                return getString(R.string.disc_dol);
            case IO_ERROR:
            default:
                return getString(R.string.disc_io);
        }
    }

    // ---- Actions --------------------------------------------------------

    private void launchGame() {
        Log.i(TAG, "launchGame valid=" + discValid + " importing=" + importing + " path=" + discPath);
        if (!discValid || discPath == null || importing) return;
        saveUiToSettings();
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("disc_path", discPath);
        intent.putExtra("disc_name", discNameText);
        startActivity(intent);
    }

    private void showAbout() {
        new MaterialAlertDialogBuilder(this)
                .setTitle(R.string.about_dialog_title)
                .setMessage(R.string.about_dialog_body)
                .setPositiveButton(R.string.close, null)
                .show();
    }

    private void confirmReset() {
        new MaterialAlertDialogBuilder(this)
                .setMessage(R.string.reset_confirm)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.reset_button, (dialog, which) -> resetSettings())
                .show();
    }

    private void resetSettings() {
        settings.clear();
        settings.put("disc", discPath == null ? "" : discPath);
        settings.put("fullscreen", 1);
        settings.put("skip_launcher", 0);
        try {
            settings.save();
        } catch (IOException e) {
            showError(getString(R.string.copy_failed, e.getMessage()));
            return;
        }
        applySettingsToUi();
        Toast.makeText(this, R.string.reset_done, Toast.LENGTH_SHORT).show();
    }

    private void showError(String message) {
        new MaterialAlertDialogBuilder(this)
                .setTitle(R.string.error_title)
                .setMessage(message)
                .setPositiveButton(R.string.close, null)
                .show();
    }

    // ---- File helpers ---------------------------------------------------

    private static String queryName(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0))
                return cursor.getString(0);
        } catch (Exception ignored) {
        }
        return null;
    }

    private static long querySize(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri,
                new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0))
                return cursor.getLong(0);
        } catch (Exception ignored) {
        }
        try (android.content.res.AssetFileDescriptor afd = resolver.openAssetFileDescriptor(uri, "r")) {
            if (afd != null) return afd.getLength();
        } catch (Exception ignored) {
        }
        return 0;
    }

    private String formatBytes(long bytes) {
        return android.text.format.Formatter.formatFileSize(this, bytes);
    }

    @Override
    protected void onPause() {
        saveUiToSettings();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        destroyed.set(true);
        io.shutdownNow();
        super.onDestroy();
    }
}
