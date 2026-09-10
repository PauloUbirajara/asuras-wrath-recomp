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

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class AsuraActivity extends SDLActivity {
    private static final int REQUEST_CODE_STORAGE_PERMISSION = 1000;
    private static final int REQUEST_CODE_PICK_ISO = 1001;
    private static final int REQUEST_CODE_PICK_FOLDER = 1002;
    private static final int REQUEST_CODE_PICK_DRIVER_ZIP = 1003;
    private static final int REQUEST_CODE_PICK_DRIVER_FOLDER = 1004;

    private static final String PREF_KEY_STORAGE_PERMISSION = "storage_permission";
    private static final String PREF_KEY_CUSTOM_DIR = "custom_game_dir";
    private static final String PREF_KEY_CUSTOM_FLAGS = "custom_cmdline_flags";
    private static final String PREF_KEY_ADRENO_DRIVER_DIR = "adreno_driver_dir";
    private static final String PREF_KEY_ADRENO_DRIVER_NAME = "adreno_driver_name";

    private boolean mPickerOpened = false;
    private Uri mPendingIsoUri = null;

    private View mSplashOverlay = null;
    private TextView mTvPermissionStatus = null;
    private TextView mTvFolderStatus = null;
    private TextView mTvGameStatus = null;
    private TextView mTvDriverStatus = null;
    private LinearLayout mFlagsListContainer = null;
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

        // Custom GPU Driver flags
        String driverDir = prefs.getString(PREF_KEY_ADRENO_DRIVER_DIR, "");
        String driverName = prefs.getString(PREF_KEY_ADRENO_DRIVER_NAME, "libvulkan_freedreno.so");
        if (driverDir != null && !driverDir.trim().isEmpty()) {
            argsList.add("--adreno_driver_path=" + driverDir.trim());
            argsList.add("--adreno_driver_name=" + driverName.trim());
        }

        // Custom command-line flags
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
        prefs.edit().putString(PREF_KEY_CUSTOM_DIR, dir.getAbsolutePath()).commit();
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

    private void openDriverZipPicker() {
        Toast.makeText(this, "Select custom Adreno driver .zip package", Toast.LENGTH_SHORT).show();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_CODE_PICK_DRIVER_ZIP);
    }

    private void openDriverFolderPicker() {
        Toast.makeText(this, "Select extracted custom driver folder", Toast.LENGTH_SHORT).show();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_CODE_PICK_DRIVER_FOLDER);
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
            return;
        }

        if (requestCode == REQUEST_CODE_PICK_DRIVER_ZIP) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                extractDriverZipInBackground(data.getData());
                return;
            }
            mPickerOpened = false;
            Toast.makeText(this, "Driver selection canceled.", Toast.LENGTH_SHORT).show();
            return;
        }

        if (requestCode == REQUEST_CODE_PICK_DRIVER_FOLDER) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri folderUri = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(folderUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {}
                File targetDir = getFileFromTreeUri(folderUri);
                File searchFile = findDriverSo(targetDir);
                String driverName = searchFile != null ? searchFile.getName() : "libvulkan_freedreno.so";
                File driverDir = searchFile != null ? searchFile.getParentFile() : targetDir;

                SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
                prefs.edit()
                    .putString(PREF_KEY_ADRENO_DRIVER_DIR, driverDir.getAbsolutePath())
                    .putString(PREF_KEY_ADRENO_DRIVER_NAME, driverName)
                    .commit();

                mPickerOpened = false;
                Toast.makeText(this, "Custom driver folder set: " + driverDir.getAbsolutePath(), Toast.LENGTH_SHORT).show();
                updateSplashStatus();
                return;
            }
            mPickerOpened = false;
            Toast.makeText(this, "Driver folder selection canceled.", Toast.LENGTH_SHORT).show();
        }
    }

    private File findDriverSo(File dir) {
        if (dir == null || !dir.exists()) return null;
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File f : files) {
            if (f.isFile() && f.getName().endsWith(".so")) {
                return f;
            }
        }
        for (File f : files) {
            if (f.isDirectory()) {
                File found = findDriverSo(f);
                if (found != null) return found;
            }
        }
        return null;
    }

    private void extractDriverZipInBackground(Uri zipUri) {
        if (zipUri == null) return;
        ProgressDialog progressDialog = new ProgressDialog(this);
        progressDialog.setTitle("Installing GPU Driver");
        progressDialog.setMessage("Extracting custom driver zip...");
        progressDialog.setIndeterminate(true);
        progressDialog.setCancelable(false);
        progressDialog.show();

        Handler mainHandler = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            String errorMsg = null;
            File targetDir = null;
            String foundDriverName = "libvulkan_freedreno.so";
            try {
                File gameDir = getGameFilesDir();
                File driversDir = new File(gameDir, "custom_drivers");
                if (!driversDir.exists()) driversDir.mkdirs();

                String zipName = "driver_" + System.currentTimeMillis();
                targetDir = new File(driversDir, zipName);
                if (!targetDir.exists()) targetDir.mkdirs();

                try (InputStream is = getContentResolver().openInputStream(zipUri);
                     ZipInputStream zis = new ZipInputStream(new BufferedInputStream(is))) {
                    ZipEntry entry;
                    byte[] buffer = new byte[8192];
                    while ((entry = zis.getNextEntry()) != null) {
                        String entryName = entry.getName();
                        if (entry.isDirectory()) {
                            new File(targetDir, entryName).mkdirs();
                            continue;
                        }
                        File destFile = new File(targetDir, entryName);
                        File parent = destFile.getParentFile();
                        if (parent != null && !parent.exists()) parent.mkdirs();
                        try (FileOutputStream fos = new FileOutputStream(destFile);
                             BufferedOutputStream bos = new BufferedOutputStream(fos, buffer.length)) {
                            int len;
                            while ((len = zis.read(buffer)) > 0) {
                                bos.write(buffer, 0, len);
                            }
                            bos.flush();
                        }
                        if (entryName.endsWith(".so")) {
                            foundDriverName = destFile.getName();
                        }
                    }
                }

                File searchFile = findDriverSo(targetDir);
                if (searchFile != null) {
                    targetDir = searchFile.getParentFile();
                    foundDriverName = searchFile.getName();
                }
            } catch (Exception e) {
                e.printStackTrace();
                errorMsg = e.getMessage();
            }

            final String err = errorMsg;
            final File finalTargetDir = targetDir;
            final String finalDriverName = foundDriverName;

            mainHandler.post(() -> {
                progressDialog.dismiss();
                mPickerOpened = false;
                if (err != null || finalTargetDir == null) {
                    Toast.makeText(AsuraActivity.this, "Failed to install driver: " + err, Toast.LENGTH_LONG).show();
                    return;
                }
                SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
                prefs.edit()
                    .putString(PREF_KEY_ADRENO_DRIVER_DIR, finalTargetDir.getAbsolutePath())
                    .putString(PREF_KEY_ADRENO_DRIVER_NAME, finalDriverName)
                    .commit();
                Toast.makeText(AsuraActivity.this, "Custom driver installed: " + finalDriverName, Toast.LENGTH_SHORT).show();
                updateSplashStatus();
            });
        }).start();
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

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dpToPx(24), dpToPx(16), dpToPx(24), dpToPx(16));

        // Safe Area WindowInsets handling for status bar, notification bar, cutouts
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int top = 0, bottom = 0, left = 0, right = 0;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                android.graphics.Insets sInsets = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout()
                );
                top = sInsets.top;
                bottom = sInsets.bottom;
                left = sInsets.left;
                right = sInsets.right;
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                top = insets.getSystemWindowInsetTop();
                bottom = insets.getSystemWindowInsetBottom();
                left = insets.getSystemWindowInsetLeft();
                right = insets.getSystemWindowInsetRight();
            }
            v.setPadding(dpToPx(20) + left, dpToPx(16) + top, dpToPx(20) + right, dpToPx(16) + bottom);
            return insets;
        });

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
        mTvDriverStatus = createStatusTextView();

        card.addView(mTvPermissionStatus);
        card.addView(mTvFolderStatus);
        card.addView(mTvGameStatus);
        card.addView(mTvDriverStatus);

        // Buttons Bar (3 setup buttons arranged vertically)
        LinearLayout btnBar = new LinearLayout(this);
        btnBar.setOrientation(LinearLayout.VERTICAL);
        btnBar.setPadding(0, dpToPx(12), 0, dpToPx(12));

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

        // Collapsible Advanced Settings & GPU Drivers Section
        final LinearLayout advContainer = new LinearLayout(this);
        advContainer.setOrientation(LinearLayout.VERTICAL);
        advContainer.setPadding(dpToPx(12), dpToPx(10), dpToPx(12), dpToPx(10));
        advContainer.setVisibility(View.GONE);

        GradientDrawable advBg = new GradientDrawable();
        advBg.setColor(0xFF1E293B);
        advBg.setCornerRadius(dpToPx(10));
        advBg.setStroke(dpToPx(1), 0xFF334155);
        advContainer.setBackground(advBg);

        final Button btnAdvToggle = createSecondaryButton("⚙ Advanced Settings & GPU Drivers  ▼");
        btnAdvToggle.setOnClickListener(v -> {
            boolean visible = advContainer.getVisibility() == View.VISIBLE;
            advContainer.setVisibility(visible ? View.GONE : View.VISIBLE);
            btnAdvToggle.setText(visible ? "⚙ Advanced Settings & GPU Drivers  ▼" : "⚙ Advanced Settings & GPU Drivers  ▲");
        });

        // Adreno Tools / Custom Driver Section
        TextView tvDriverLabel = new TextView(this);
        tvDriverLabel.setText("🎮 Custom GPU Driver (Adreno Tools):");
        tvDriverLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tvDriverLabel.setTextColor(0xFFE2E8F0);
        tvDriverLabel.setTypeface(Typeface.DEFAULT_BOLD);
        tvDriverLabel.setPadding(0, 0, 0, dpToPx(6));

        Button btnDriverZip = createSecondaryButton("Select Driver (.zip)");
        btnDriverZip.setOnClickListener(v -> {
            mPickerOpened = true;
            openDriverZipPicker();
        });

        Button btnDriverFolder = createSecondaryButton("Select Driver Folder");
        btnDriverFolder.setOnClickListener(v -> {
            mPickerOpened = true;
            openDriverFolderPicker();
        });

        Button btnDriverReset = createSecondaryButton("Reset to System Driver");
        btnDriverReset.setOnClickListener(v -> {
            SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
            prefs.edit().remove(PREF_KEY_ADRENO_DRIVER_DIR).remove(PREF_KEY_ADRENO_DRIVER_NAME).commit();
            Toast.makeText(this, "Reset to system Vulkan driver.", Toast.LENGTH_SHORT).show();
            updateSplashStatus();
        });

        LinearLayout.LayoutParams drvBtnParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(40));
        drvBtnParams.setMargins(0, dpToPx(3), 0, dpToPx(3));

        advContainer.addView(tvDriverLabel);
        advContainer.addView(btnDriverZip, drvBtnParams);
        advContainer.addView(btnDriverFolder, drvBtnParams);
        advContainer.addView(btnDriverReset, drvBtnParams);

        // Divider in Advanced Settings
        View advDiv = new View(this);
        advDiv.setBackgroundColor(0xFF334155);
        LinearLayout.LayoutParams advDivParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(1));
        advDivParams.setMargins(0, dpToPx(10), 0, dpToPx(10));
        advContainer.addView(advDiv, advDivParams);

        // Key-Value Flags UI Header
        TextView tvFlagsHeader = new TextView(this);
        tvFlagsHeader.setText("🚩 Command-Line Flags (Key-Value):");
        tvFlagsHeader.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tvFlagsHeader.setTextColor(0xFFE2E8F0);
        tvFlagsHeader.setTypeface(Typeface.DEFAULT_BOLD);
        tvFlagsHeader.setPadding(0, 0, 0, dpToPx(6));

        mFlagsListContainer = new LinearLayout(this);
        mFlagsListContainer.setOrientation(LinearLayout.VERTICAL);

        Button btnAddFlag = createSecondaryButton("➕ Add Flag");
        btnAddFlag.setOnClickListener(v -> addFlagRow("", ""));

        advContainer.addView(tvFlagsHeader);
        advContainer.addView(mFlagsListContainer);
        advContainer.addView(btnAddFlag, drvBtnParams);

        // Load saved flags into list UI
        SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
        String savedFlags = prefs.getString(PREF_KEY_CUSTOM_FLAGS, "");
        loadSavedFlagsIntoList(savedFlags);

        // Separator / Divider
        View divider = new View(this);
        divider.setBackgroundColor(0xFF334155);
        LinearLayout.LayoutParams divParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(1));
        divParams.setMargins(0, dpToPx(12), 0, dpToPx(16));

        // Play Button (Separated from the top setup buttons)
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
        root.addView(btnAdvToggle, btnParams);
        root.addView(advContainer);
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

    private void addFlagRow(String key, String value) {
        if (mFlagsListContainer == null) {
            return;
        }
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dpToPx(4), 0, dpToPx(4));

        EditText etKey = new EditText(this);
        etKey.setHint("Key (e.g. video_mode_height)");
        etKey.setHintTextColor(0xFF64748B);
        etKey.setTextColor(0xFFF8FAFC);
        etKey.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        etKey.setText(key != null ? key : "");
        etKey.setSingleLine(true);
        etKey.setPadding(dpToPx(8), dpToPx(6), dpToPx(8), dpToPx(6));

        GradientDrawable etBg1 = new GradientDrawable();
        etBg1.setColor(0xFF0F172A);
        etBg1.setCornerRadius(dpToPx(6));
        etBg1.setStroke(dpToPx(1), 0xFF475569);
        etKey.setBackground(etBg1);

        TextView tvEq = new TextView(this);
        tvEq.setText(" = ");
        tvEq.setTextColor(0xFF94A3B8);
        tvEq.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);

        EditText etVal = new EditText(this);
        etVal.setHint("Value (e.g. 720)");
        etVal.setHintTextColor(0xFF64748B);
        etVal.setTextColor(0xFFF8FAFC);
        etVal.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        etVal.setText(value != null ? value : "");
        etVal.setSingleLine(true);
        etVal.setPadding(dpToPx(8), dpToPx(6), dpToPx(8), dpToPx(6));

        GradientDrawable etBg2 = new GradientDrawable();
        etBg2.setColor(0xFF0F172A);
        etBg2.setCornerRadius(dpToPx(6));
        etBg2.setStroke(dpToPx(1), 0xFF475569);
        etVal.setBackground(etBg2);

        Button btnRemove = new Button(this);
        btnRemove.setText("✕");
        btnRemove.setTextColor(0xFFF87171);
        btnRemove.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        btnRemove.setTypeface(Typeface.DEFAULT_BOLD);

        GradientDrawable remBg = new GradientDrawable();
        remBg.setColor(0xFF334155);
        remBg.setCornerRadius(dpToPx(6));
        btnRemove.setBackground(remBg);
        btnRemove.setOnClickListener(v -> mFlagsListContainer.removeView(row));

        LinearLayout.LayoutParams kParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
        LinearLayout.LayoutParams vParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
        LinearLayout.LayoutParams btnParams = new LinearLayout.LayoutParams(dpToPx(36), dpToPx(36));
        btnParams.setMargins(dpToPx(6), 0, 0, 0);

        row.addView(etKey, kParams);
        row.addView(tvEq);
        row.addView(etVal, vParams);
        row.addView(btnRemove, btnParams);

        mFlagsListContainer.addView(row);
    }

    private void saveFlagsFromList() {
        if (mFlagsListContainer == null) {
            return;
        }
        StringBuilder sb = new StringBuilder();
        int count = mFlagsListContainer.getChildCount();
        for (int i = 0; i < count; i++) {
            View child = mFlagsListContainer.getChildAt(i);
            if (child instanceof LinearLayout) {
                LinearLayout row = (LinearLayout) child;
                if (row.getChildCount() >= 4) {
                    View vKey = row.getChildAt(0);
                    View vVal = row.getChildAt(2);
                    if (vKey instanceof EditText && vVal instanceof EditText) {
                        String k = ((EditText) vKey).getText().toString().trim();
                        String v = ((EditText) vVal).getText().toString().trim();
                        if (!k.isEmpty()) {
                            if (!k.startsWith("--")) {
                                k = "--" + k;
                            }
                            if (sb.length() > 0) {
                                sb.append(" ");
                            }
                            if (!v.isEmpty()) {
                                sb.append(k).append("=").append(v);
                            } else {
                                sb.append(k);
                            }
                        }
                    }
                }
            }
        }
        SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
        prefs.edit().putString(PREF_KEY_CUSTOM_FLAGS, sb.toString()).commit();
    }

    private void loadSavedFlagsIntoList(String savedFlags) {
        if (mFlagsListContainer == null) {
            return;
        }
        mFlagsListContainer.removeAllViews();
        if (savedFlags == null || savedFlags.trim().isEmpty()) {
            return;
        }
        String[] tokens = savedFlags.trim().split("\\s+");
        for (String token : tokens) {
            if (token.isEmpty()) continue;
            String cleanToken = token.startsWith("--") ? token.substring(2) : token;
            int eq = cleanToken.indexOf('=');
            if (eq != -1) {
                String k = cleanToken.substring(0, eq);
                String v = cleanToken.substring(eq + 1);
                addFlagRow(k, v);
            } else {
                addFlagRow(cleanToken, "");
            }
        }
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

        if (mTvDriverStatus != null) {
            SharedPreferences prefs = getSharedPreferences("asura_prefs", Context.MODE_PRIVATE);
            String driverDir = prefs.getString(PREF_KEY_ADRENO_DRIVER_DIR, null);
            String driverName = prefs.getString(PREF_KEY_ADRENO_DRIVER_NAME, "libvulkan_freedreno.so");
            if (driverDir != null && !driverDir.trim().isEmpty()) {
                mTvDriverStatus.setText("⚡ GPU Driver: Custom (" + driverName + ")");
                mTvDriverStatus.setTextColor(0xFF38BDF8);
            } else {
                mTvDriverStatus.setText("⚡ GPU Driver: System Vulkan (Default)");
                mTvDriverStatus.setTextColor(0xFF94A3B8);
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

        saveFlagsFromList();

        mGameStarted = true;
        if (mSplashOverlay != null) {
            mSplashOverlay.setVisibility(View.GONE);
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        setFullscreenImmersive();
        super.resumeNativeThread();
    }
}
