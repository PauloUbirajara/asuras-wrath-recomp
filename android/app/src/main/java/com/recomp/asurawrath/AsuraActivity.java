package com.recomp.asurawrath;

import org.libsdl.app.SDLActivity;

import android.Manifest;
import android.app.Activity;
import android.app.ProgressDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

public class AsuraActivity extends SDLActivity {
    private static final int REQUEST_CODE_STORAGE_PERMISSION = 1000;
    private static final int REQUEST_CODE_PICK_ISO = 1001;
    private static final int REQUEST_CODE_PICK_FOLDER = 1002;
    private static final String PREF_KEY_STORAGE_PERMISSION = "storage_permission";
    private static final String PREF_KEY_CUSTOM_DIR = "custom_game_dir";
    private static final String PREF_KEY_CUSTOM_FLAGS = "custom_cmdline_flags";

    private boolean mPickerOpened = false;
    private Uri mPendingIsoUri = null;

    private View mSplashOverlay = null;
    private TextView mTvPermissionStatus = null;
    private TextView mTvFolderStatus = null;
    private TextView mTvGameStatus = null;
    private android.widget.EditText mEtFlags = null;
    private Button mBtnPlay = null;
    private boolean mGameStarted = false;
    private long mLastBackPressTime = 0;

    static {
        System.setProperty("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1");
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "rexruntime",
            "rexgpu-xenos",
            "asura_wrath_recomp"
        };
    }

    @Override
    protected String[] getArguments() {
        File dataDir = getGameFilesDir();
        File cacheDir = new File(dataDir, "cache");
        File logsDir = new File(dataDir, "logs");
        if (!cacheDir.exists()) {
            cacheDir.mkdirs();
        }
        if (!logsDir.exists()) {
            logsDir.mkdirs();
        }
        java.util.ArrayList<String> argsList = new java.util.ArrayList<>();
        argsList.add("--user_data_root=" + dataDir.getAbsolutePath());
        argsList.add("--game_data_root=" + dataDir.getAbsolutePath());
        argsList.add("--cache_root=" + cacheDir.getAbsolutePath());

        SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
        String customFlags = prefs.getString(PREF_KEY_CUSTOM_FLAGS, "");
        if (customFlags != null && !customFlags.trim().isEmpty()) {
            String[] userFlags = customFlags.trim().split("\\s+");
            for (String flag : userFlags) {
                if (!flag.isEmpty()) {
                    argsList.add(flag);
                }
            }
        }
        return argsList.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (mGameStarted) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        } else {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        }
        super.onCreate(savedInstanceState);
        setFullscreenImmersive();
        createSplashUI();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                this::handleBackPress
            );
        }
    }

    private void handleBackPress() {
        long now = System.currentTimeMillis();
        if (now - mLastBackPressTime < 2000) {
            finish();
            System.exit(0);
        } else {
            mLastBackPressTime = now;
            Toast.makeText(this, "Press back again to exit game", Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    public void onBackPressed() {
        handleBackPress();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP && event.getRepeatCount() == 0) {
                handleBackPress();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mActivityCreated = false;
        mSDLMainFinished = false;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            setFullscreenImmersive();
        }
    }

    private void setFullscreenImmersive() {
        if (getWindow() == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            View decorView = getWindow().getDecorView();
            if (decorView != null) {
                int flags = View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
                decorView.setSystemUiVisibility(flags);
            }
        }
    }

    @Override
    protected void resumeNativeThread() {
        if (!mGameStarted) {
            return;
        }
        File filesDir = getGameFilesDir();
        if (!hasStoragePermission()) {
            return;
        }
        if (!hasGameFiles(filesDir)) {
            return;
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.resumeNativeThread();
    }

    private File getGameFilesDir() {
        SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
        String customPath = prefs.getString(PREF_KEY_CUSTOM_DIR, null);
        if (customPath != null) {
            File customDir = new File(customPath);
            if (customDir.exists() || customDir.mkdirs()) {
                return customDir;
            }
        }
        File filesDir = getExternalFilesDir(null);
        return filesDir != null ? filesDir : getFilesDir();
    }

    private void saveGameFilesDir(File dir) {
        if (dir == null) {
            return;
        }
        SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
        prefs.edit().putString(PREF_KEY_CUSTOM_DIR, dir.getAbsolutePath()).apply();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mGameStarted) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            super.resumeNativeThread();
            return;
        }
        updateSplashStatus();
    }

    private boolean hasStoragePermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    private void requestStoragePermission() {
        Toast.makeText(this, "Storage permission is required for game files", Toast.LENGTH_LONG).show();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                startActivityForResult(intent, REQUEST_CODE_STORAGE_PERMISSION);
                return;
            } catch (Exception e) {
                Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                startActivityForResult(intent, REQUEST_CODE_STORAGE_PERMISSION);
                return;
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            requestPermissions(new String[]{
                Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            }, REQUEST_CODE_STORAGE_PERMISSION);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        mPickerOpened = false;
        if (requestCode != REQUEST_CODE_STORAGE_PERMISSION) {
            return;
        }
        if (hasStoragePermission()) {
            onResume();
            return;
        }
        Toast.makeText(this, "Storage permission denied.", Toast.LENGTH_LONG).show();
    }

    private boolean hasGameFiles(File dir) {
        if (dir == null || !dir.exists()) {
            return false;
        }
        if (checkDirectoryForGameFiles(dir)) {
            return true;
        }
        File subDir = new File(dir, "asura_wrath_recomp");
        return checkDirectoryForGameFiles(subDir);
    }

    private boolean checkDirectoryForGameFiles(File dir) {
        if (dir == null || !dir.exists()) {
            return false;
        }
        if (new File(dir, "default.xex").isFile()) {
            return true;
        }
        File extractedDir = new File(dir, "extracted");
        if (extractedDir.isDirectory() && checkDirectoryForGameFiles(extractedDir)) {
            return true;
        }
        File gameDataDir = new File(dir, "game_data");
        if (gameDataDir.isDirectory() && checkDirectoryForGameFiles(gameDataDir)) {
            return true;
        }
        File[] files = dir.listFiles();
        if (files == null) {
            return false;
        }
        for (File f : files) {
            if (f.isFile()) {
                String name = f.getName().toLowerCase();
                if (name.endsWith(".iso") || name.endsWith(".gdfx")) {
                    return true;
                }
            }
        }
        return false;
    }

    private void openIsoPicker() {
        Toast.makeText(this, "Select your Asura's Wrath Xbox 360 ISO", Toast.LENGTH_SHORT).show();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_CODE_PICK_ISO);
    }

    private void openFolderPicker() {
        Toast.makeText(this, "Select destination folder for game, cache & logs", Toast.LENGTH_SHORT).show();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION 
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION 
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_CODE_PICK_FOLDER);
    }

    private File getFileFromTreeUri(Uri uri) {
        if (uri == null) {
            return getExternalFilesDir(null);
        }
        try {
            String docId = DocumentsContract.getTreeDocumentId(uri);
            if (docId != null) {
                String[] split = docId.split(":");
                String type = split[0];
                String path = split.length > 1 ? split[1] : "";
                if ("primary".equalsIgnoreCase(type)) {
                    File root = Environment.getExternalStorageDirectory();
                    return path.isEmpty() ? root : new File(root, path);
                } else {
                    File root = new File("/storage/" + type);
                    if (root.exists()) {
                        return path.isEmpty() ? root : new File(root, path);
                    }
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return getExternalFilesDir(null);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CODE_STORAGE_PERMISSION) {
            mPickerOpened = false;
            if (hasStoragePermission()) {
                onResume();
                return;
            }
            Toast.makeText(this, "Storage permission required.", Toast.LENGTH_LONG).show();
            updateSplashStatus();
            return;
        }

        if (requestCode == REQUEST_CODE_PICK_ISO) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                mPendingIsoUri = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(mPendingIsoUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {}
                File targetDir = getGameFilesDir();
                copyIsoInBackground(mPendingIsoUri, targetDir);
                return;
            }
            mPickerOpened = false;
            Toast.makeText(this, "ISO selection canceled.", Toast.LENGTH_SHORT).show();
            updateSplashStatus();
            return;
        }

        if (requestCode == REQUEST_CODE_PICK_FOLDER) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri folderUri = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(folderUri, 
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                } catch (Exception ignored) {}

                File targetDir = getFileFromTreeUri(folderUri);
                saveGameFilesDir(targetDir);
                mPickerOpened = false;
                Toast.makeText(this, "Target folder set: " + targetDir.getAbsolutePath(), Toast.LENGTH_SHORT).show();
                updateSplashStatus();
                return;
            }
            mPickerOpened = false;
            Toast.makeText(this, "Folder selection canceled.", Toast.LENGTH_SHORT).show();
            updateSplashStatus();
        }
    }

    private void copyIsoInBackground(Uri uri, File targetDir) {
        if (targetDir == null) {
            targetDir = getGameFilesDir();
        }
        if (!targetDir.exists()) {
            targetDir.mkdirs();
        }

        File cacheDir = new File(targetDir, "cache");
        File logsDir = new File(targetDir, "logs");
        if (!cacheDir.exists()) cacheDir.mkdirs();
        if (!logsDir.exists()) logsDir.mkdirs();

        long totalBytes = -1;
        if (uri != null) {
            try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                if (pfd != null) {
                    totalBytes = pfd.getStatSize();
                }
            } catch (Exception ignored) {}
        }

        final long finalTotalBytes = totalBytes;
        final File destDir = targetDir;

        ProgressDialog progressDialog = new ProgressDialog(this);
        progressDialog.setTitle("Preparing Asura's Wrath");
        progressDialog.setMessage("Copying ISO to selected folder...");
        progressDialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        progressDialog.setCancelable(false);
        if (finalTotalBytes > 0) {
            progressDialog.setMax(100);
        } else {
            progressDialog.setIndeterminate(true);
        }
        progressDialog.show();

        Handler mainHandler = new Handler(Looper.getMainLooper());

        new Thread(() -> {
            String errorMessage = null;
            File extractedFolder = new File(destDir, "extracted");
            if (!extractedFolder.exists()) {
                extractedFolder.mkdirs();
            }
            File targetFile = new File(extractedFolder, "Asura's Wrath.iso");

            if (uri != null) {
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(targetFile)) {
                    if (in == null) {
                        throw new Exception("Unable to open input stream for selected file.");
                    }
                    byte[] buffer = new byte[128 * 1024];
                    int bytesRead;
                    long copiedBytes = 0;
                    long lastUpdate = 0;

                    while ((bytesRead = in.read(buffer)) != -1) {
                        out.write(buffer, 0, bytesRead);
                        copiedBytes += bytesRead;

                        long now = System.currentTimeMillis();
                        if (now - lastUpdate > 100 || (finalTotalBytes > 0 && copiedBytes == finalTotalBytes)) {
                            lastUpdate = now;
                            final long currentCopied = copiedBytes;
                            mainHandler.post(() -> {
                                if (finalTotalBytes > 0) {
                                    int percent = (int) ((currentCopied * 100) / finalTotalBytes);
                                    progressDialog.setProgress(percent);
                                    progressDialog.setMessage(String.format(Locale.US,
                                        "Copying ISO: %.1f MB / %.1f MB (%d%%)",
                                        currentCopied / (1024.0 * 1024.0),
                                        finalTotalBytes / (1024.0 * 1024.0),
                                        percent));
                                } else {
                                    progressDialog.setMessage(String.format(Locale.US,
                                        "Copying ISO: %.1f MB",
                                        currentCopied / (1024.0 * 1024.0)));
                                }
                            });
                        }
                    }
                    out.flush();
                } catch (Throwable t) {
                    t.printStackTrace();
                    errorMessage = t.getClass().getSimpleName() + ": " + t.getMessage();
                    if (targetFile.exists()) {
                        targetFile.delete();
                    }
                }
            }

            final String errorStr = errorMessage;
            mainHandler.post(() -> {
                progressDialog.dismiss();
                mPickerOpened = false;
                if (errorStr != null) {
                    Toast.makeText(AsuraActivity.this, "Failed to copy ISO: " + errorStr, Toast.LENGTH_LONG).show();
                    updateSplashStatus();
                    return;
                }
                Toast.makeText(AsuraActivity.this, "Setup complete!", Toast.LENGTH_SHORT).show();
                updateSplashStatus();
            });
        }).start();
    }

    private void createSplashUI() {
        if (mSplashOverlay != null) {
            return;
        }

        ScrollView scrollView = new ScrollView(this);
        scrollView.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dpToPx(24), dpToPx(16), dpToPx(24), dpToPx(16));

        GradientDrawable rootBg = new GradientDrawable(
            GradientDrawable.Orientation.TOP_BOTTOM,
            new int[]{0xFF0F172A, 0xFF1E1E2E}
        );
        root.setBackground(rootBg);

        // Title Header
        TextView title = new TextView(this);
        title.setText("ASURA'S WRATH RECOMPILED");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        title.setTextColor(Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);

        TextView subtitle = new TextView(this);
        subtitle.setText("Android Recompiled Port & Launcher");
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        subtitle.setTextColor(0xFF94A3B8);
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setPadding(0, dpToPx(2), 0, dpToPx(16));

        // Status Card
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dpToPx(16), dpToPx(12), dpToPx(16), dpToPx(12));

        GradientDrawable cardBg = new GradientDrawable();
        cardBg.setColor(0xFF1E293B);
        cardBg.setCornerRadius(dpToPx(10));
        cardBg.setStroke(dpToPx(1), 0xFF334155);
        card.setBackground(cardBg);

        mTvPermissionStatus = createStatusTextView();
        mTvFolderStatus = createStatusTextView();
        mTvGameStatus = createStatusTextView();

        card.addView(mTvPermissionStatus);
        card.addView(mTvFolderStatus);
        card.addView(mTvGameStatus);

        // Buttons Bar (3 setup buttons arranged vertically)
        LinearLayout btnBar = new LinearLayout(this);
        btnBar.setOrientation(LinearLayout.VERTICAL);
        btnBar.setPadding(0, dpToPx(16), 0, dpToPx(16));

        Button btnPermission = createSecondaryButton("1. Grant Storage Permission");
        btnPermission.setOnClickListener(v -> {
            mPickerOpened = true;
            requestStoragePermission();
        });

        Button btnFolder = createSecondaryButton("2. Select Target Folder");
        btnFolder.setOnClickListener(v -> {
            mPickerOpened = true;
            openFolderPicker();
        });

        Button btnIso = createSecondaryButton("3. Select & Extract ISO");
        btnIso.setOnClickListener(v -> {
            mPickerOpened = true;
            openIsoPicker();
        });

        LinearLayout.LayoutParams btnParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(44));
        btnParams.setMargins(0, dpToPx(4), 0, dpToPx(4));

        btnBar.addView(btnPermission, btnParams);
        btnBar.addView(btnFolder, btnParams);
        btnBar.addView(btnIso, btnParams);

        // Custom Command-Line Flags Card
        LinearLayout flagsCard = new LinearLayout(this);
        flagsCard.setOrientation(LinearLayout.VERTICAL);
        flagsCard.setPadding(dpToPx(12), dpToPx(10), dpToPx(12), dpToPx(10));

        GradientDrawable flagsBg = new GradientDrawable();
        flagsBg.setColor(0xFF1E293B);
        flagsBg.setCornerRadius(dpToPx(10));
        flagsBg.setStroke(dpToPx(1), 0xFF334155);
        flagsCard.setBackground(flagsBg);

        TextView tvFlagsLabel = new TextView(this);
        tvFlagsLabel.setText("⚙ Custom Command-Line Flags:");
        tvFlagsLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tvFlagsLabel.setTextColor(0xFFE2E8F0);
        tvFlagsLabel.setTypeface(Typeface.DEFAULT_BOLD);
        tvFlagsLabel.setPadding(0, 0, 0, dpToPx(6));

        mEtFlags = new EditText(this);
        mEtFlags.setHint("e.g. --enable_fsi=true --video_mode_height=720");
        mEtFlags.setHintTextColor(0xFF64748B);
        mEtFlags.setTextColor(0xFFF8FAFC);
        mEtFlags.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        mEtFlags.setPadding(dpToPx(10), dpToPx(8), dpToPx(10), dpToPx(8));
        mEtFlags.setSingleLine(false);
        mEtFlags.setMaxLines(3);

        GradientDrawable etBg = new GradientDrawable();
        etBg.setColor(0xFF0F172A);
        etBg.setCornerRadius(dpToPx(6));
        etBg.setStroke(dpToPx(1), 0xFF475569);
        mEtFlags.setBackground(etBg);

        SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
        String savedFlags = prefs.getString(PREF_KEY_CUSTOM_FLAGS, "");
        if (savedFlags != null) {
            mEtFlags.setText(savedFlags);
        }

        flagsCard.addView(tvFlagsLabel);
        flagsCard.addView(mEtFlags);

        // Separator / Divider
        View divider = new View(this);
        divider.setBackgroundColor(0xFF334155);
        LinearLayout.LayoutParams divParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(1));
        divParams.setMargins(0, dpToPx(12), 0, dpToPx(16));

        // Play Button (Separated from the top three buttons)
        mBtnPlay = new Button(this);
        mBtnPlay.setText("PLAY GAME");
        mBtnPlay.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        mBtnPlay.setTypeface(Typeface.DEFAULT_BOLD);
        mBtnPlay.setTextColor(Color.WHITE);

        GradientDrawable playBg = new GradientDrawable(
            GradientDrawable.Orientation.LEFT_RIGHT,
            new int[]{0xFFFF6B00, 0xFFE11D48}
        );
        playBg.setCornerRadius(dpToPx(12));
        mBtnPlay.setBackground(playBg);
        mBtnPlay.setOnClickListener(v -> onPlayButtonClicked());

        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(52));
        playParams.setMargins(dpToPx(16), 0, dpToPx(16), dpToPx(16));

        // Assemble Layout
        root.addView(title);
        root.addView(subtitle);
        root.addView(card);
        root.addView(btnBar);
        root.addView(flagsCard);
        root.addView(divider, divParams);
        root.addView(mBtnPlay, playParams);

        scrollView.addView(root);
        mSplashOverlay = scrollView;

        if (mLayout != null) {
            mLayout.addView(mSplashOverlay, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        } else {
            addContentView(mSplashOverlay, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }

        updateSplashStatus();
    }

    private TextView createStatusTextView() {
        TextView tv = new TextView(this);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tv.setPadding(0, dpToPx(4), 0, dpToPx(4));
        return tv;
    }

    private Button createSecondaryButton(String text) {
        Button btn = new Button(this);
        btn.setText(text);
        btn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        btn.setTextColor(0xFFE2E8F0);

        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF334155);
        bg.setCornerRadius(dpToPx(8));
        btn.setBackground(bg);
        return btn;
    }

    private int dpToPx(int dp) {
        float density = getResources().getDisplayMetrics().density;
        return Math.round(dp * density);
    }

    private void updateSplashStatus() {
        if (mSplashOverlay == null) {
            return;
        }
        boolean hasPerm = hasStoragePermission();
        File filesDir = getGameFilesDir();
        boolean hasFiles = hasGameFiles(filesDir);

        if (mTvPermissionStatus != null) {
            if (hasPerm) {
                mTvPermissionStatus.setText("✓ Storage Permission: Granted");
                mTvPermissionStatus.setTextColor(0xFF4ADE80);
            } else {
                mTvPermissionStatus.setText("✗ Storage Permission: Required");
                mTvPermissionStatus.setTextColor(0xFFF87171);
            }
        }

        if (mTvFolderStatus != null) {
            String path = (filesDir != null) ? filesDir.getAbsolutePath() : "Not Selected";
            mTvFolderStatus.setText("📁 Target Folder: " + path);
            mTvFolderStatus.setTextColor(0xFFE2E8F0);
        }

        if (mTvGameStatus != null) {
            if (hasFiles) {
                mTvGameStatus.setText("✓ Game Files: Ready to Play");
                mTvGameStatus.setTextColor(0xFF4ADE80);
            } else {
                mTvGameStatus.setText("✗ Game Files: Missing (Select Folder or Select ISO)");
                mTvGameStatus.setTextColor(0xFFF87171);
            }
        }

        if (mBtnPlay != null) {
            boolean ready = hasPerm && hasFiles;
            mBtnPlay.setAlpha(ready ? 1.0f : 0.6f);
        }
    }

    private void onPlayButtonClicked() {
        if (!hasStoragePermission()) {
            Toast.makeText(this, "Storage permission is required to play. Click '1. Permissions'.", Toast.LENGTH_LONG).show();
            requestStoragePermission();
            return;
        }
        File filesDir = getGameFilesDir();
        if (!hasGameFiles(filesDir)) {
            Toast.makeText(this, "No game files found! Click '2. Select Folder' or '3. Select ISO'.", Toast.LENGTH_LONG).show();
            return;
        }

        if (mEtFlags != null) {
            String flagsStr = mEtFlags.getText().toString();
            SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
            prefs.edit().putString(PREF_KEY_CUSTOM_FLAGS, flagsStr).apply();
        }

        mGameStarted = true;
        if (mSplashOverlay != null) {
            mSplashOverlay.setVisibility(View.GONE);
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        setFullscreenImmersive();
        super.resumeNativeThread();
    }
}
