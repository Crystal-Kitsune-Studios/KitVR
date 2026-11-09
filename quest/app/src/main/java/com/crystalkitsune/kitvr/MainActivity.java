package com.crystalkitsune.kitvr;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.content.Context;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final String RELEASE_URL =
            "https://api.github.com/repos/Crystal-Kitsune-Studios/KitVR/releases/latest";
    private static final String APK_ASSET = "app-release.apk";
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private EditText hostInput;
    private TextView status;
    private TextView updateStatus;
    private Button updateButton;
    private String updateUrl;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        buildUi();
        checkForUpdate();
    }

    private void buildUi() {
        int background = Color.rgb(16, 23, 25);
        int panel = Color.rgb(27, 38, 39);
        int accent = Color.rgb(124, 220, 174);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(dp(36), dp(24), dp(36), dp(24));
        root.setBackgroundColor(background);

        ScrollView scroll = new ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), dp(24), dp(28), dp(24));
        content.setBackgroundColor(panel);
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(dp(580), ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = label("KITVR", 30, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        content.addView(title);
        TextView subtitle = label("PC VR streaming", 16, accent);
        subtitle.setPadding(0, dp(2), 0, dp(22));
        content.addView(subtitle);

        content.addView(label("PC ADDRESS", 12, Color.rgb(173, 190, 185)));
        hostInput = new EditText(this);
        hostInput.setSingleLine(true);
        hostInput.setHint("e.g. 192.168.1.50");
        hostInput.setTextColor(Color.WHITE);
        hostInput.setHintTextColor(Color.rgb(134, 151, 147));
        hostInput.setTextSize(18);
        hostInput.setInputType( android.text.InputType.TYPE_CLASS_TEXT |
                android.text.InputType.TYPE_TEXT_VARIATION_URI);
        hostInput.setPadding(dp(14), dp(10), dp(14), dp(10));
        hostInput.setBackgroundTintList(android.content.res.ColorStateList.valueOf(accent));
        hostInput.setText(getPreferences(MODE_PRIVATE).getString("host", ""));
        content.addView(hostInput, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(54)));

        Button connect = button("Connect", accent, background);
        LinearLayout.LayoutParams connectParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(52));
        connectParams.topMargin = dp(18);
        content.addView(connect, connectParams);
        connect.setOnClickListener(v -> connect());

        status = label("Enter your PC's local IP address to start streaming.", 14,
                Color.rgb(190, 203, 198));
        status.setPadding(0, dp(12), 0, dp(22));
        content.addView(status);

        View divider = new View(this);
        divider.setBackgroundColor(Color.rgb(58, 76, 72));
        content.addView(divider, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));

        TextView updateTitle = label("UPDATES", 12, Color.rgb(173, 190, 185));
        updateTitle.setPadding(0, dp(18), 0, dp(4));
        content.addView(updateTitle);
        updateStatus = label("Checking for updates...", 14, Color.rgb(190, 203, 198));
        content.addView(updateStatus);
        updateButton = button("Download and install", accent, background);
        updateButton.setVisibility(View.GONE);
        LinearLayout.LayoutParams updateParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(48));
        updateParams.topMargin = dp(12);
        content.addView(updateButton, updateParams);
        updateButton.setOnClickListener(v -> downloadAndInstall());

        setContentView(root);
    }

    private void connect() {
        String host = hostInput.getText().toString().trim();
        if (host.isEmpty()) {
            hostInput.setError("Enter your PC's IP address");
            return;
        }
        getPreferences(MODE_PRIVATE).edit().putString("host", host).apply();
        ((InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE))
                .hideSoftInputFromWindow(hostInput.getWindowToken(), 0);
        status.setText("Starting KitVR. Make sure the KitVR server is running on " + host + ".");
        Intent intent = new Intent(this, android.app.NativeActivity.class);
        intent.putExtra("host", host);
        startActivity(intent);
    }

    private void checkForUpdate() {
        worker.execute(() -> {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(RELEASE_URL).openConnection();
                connection.setConnectTimeout(8000);
                connection.setReadTimeout(8000);
                connection.setRequestProperty("Accept", "application/vnd.github+json");
                connection.setRequestProperty("User-Agent", "KitVR-Quest");
                JSONObject release = new JSONObject(readResponse(connection.getInputStream()));
                String latest = release.getString("tag_name").replaceFirst("^v", "");
                String current = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                JSONArray assets = release.getJSONArray("assets");
                String apkUrl = null;
                for (int i = 0; i < assets.length(); i++) {
                    JSONObject asset = assets.getJSONObject(i);
                    if (APK_ASSET.equals(asset.getString("name"))) {
                        apkUrl = asset.getString("browser_download_url");
                        break;
                    }
                }
                if (apkUrl == null) throw new IllegalStateException("Release APK not found");
                if (compareVersions(latest, current) > 0) {
                    String downloadUrl = apkUrl;
                    runOnUiThread(() -> {
                        updateUrl = downloadUrl;
                        updateStatus.setText("Version " + latest + " is available (installed: " + current + ").");
                        updateButton.setVisibility(View.VISIBLE);
                    });
                } else {
                    runOnUiThread(() -> updateStatus.setText("You're up to date (" + current + ")."));
                }
            } catch (Exception e) {
                runOnUiThread(() -> updateStatus.setText("Couldn't check for updates. Check your internet connection."));
            } finally {
                if (connection != null) connection.disconnect();
            }
        });
    }

    private void downloadAndInstall() {
        if (updateUrl == null) return;
        updateButton.setEnabled(false);
        updateStatus.setText("Downloading update...");
        worker.execute(() -> {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(updateUrl).openConnection();
                connection.setConnectTimeout(15000);
                connection.setReadTimeout(30000);
                File directory = new File(getCacheDir(), "updates");
                if (!directory.exists() && !directory.mkdirs()) throw new IllegalStateException("Cannot create update folder");
                File apk = new File(directory, APK_ASSET);
                try (InputStream input = connection.getInputStream();
                     FileOutputStream output = new FileOutputStream(apk)) {
                    byte[] buffer = new byte[16384];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                runOnUiThread(() -> installApk(apk));
            } catch (Exception e) {
                runOnUiThread(() -> {
                    updateStatus.setText("Download failed. Try again when online.");
                    updateButton.setEnabled(true);
                });
            } finally {
                if (connection != null) connection.disconnect();
            }
        });
    }

    private void installApk(File apk) {
        if (android.os.Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
            updateStatus.setText("Allow KitVR to install updates, then tap Download and install again.");
            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + getPackageName())));
            updateButton.setEnabled(true);
            return;
        }
        Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".updates", apk);
        Intent install = new Intent(Intent.ACTION_VIEW);
        install.setDataAndType(uri, "application/vnd.android.package-archive");
        install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(install);
        updateStatus.setText("Android will ask you to confirm the update.");
        updateButton.setEnabled(true);
    }

    private static int compareVersions(String left, String right) {
        String[] a = left.split("[.-]");
        String[] b = right.split("[.-]");
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? numericPart(a[i]) : 0;
            int y = i < b.length ? numericPart(b[i]) : 0;
            if (x != y) return Integer.compare(x, y);
        }
        return 0;
    }

    private static int numericPart(String value) {
        String digits = value.replaceAll("[^0-9].*$", "");
        try { return digits.isEmpty() ? 0 : Integer.parseInt(digits); }
        catch (NumberFormatException ignored) { return 0; }
    }

    private static String readResponse(InputStream input) throws Exception {
        try (InputStream stream = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = stream.read(buffer)) != -1) output.write(buffer, 0, count);
            return output.toString("UTF-8");
        }
    }

    private TextView label(String text, int size, int color) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private Button button(String text, int color, int textColor) {
        Button view = new Button(this);
        view.setText(text);
        view.setTextColor(textColor);
        view.setAllCaps(false);
        view.setTextSize(16);
        view.setBackgroundTintList(android.content.res.ColorStateList.valueOf(color));
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        worker.shutdownNow();
    }
}
