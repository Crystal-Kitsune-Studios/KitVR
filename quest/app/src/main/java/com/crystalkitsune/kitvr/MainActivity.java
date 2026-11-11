package com.crystalkitsune.kitvr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.URL;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final String RELEASE_URL =
            "https://api.github.com/repos/Crystal-Kitsune-Studios/KitVR/releases/latest";
    private static final String APK_ASSET = "app-release.apk";
    private static final int REQUEST_TRACKING_PERMISSIONS = 3;
    private static final String HAND_TRACKING_PERMISSION = "com.oculus.permission.HAND_TRACKING";
    private static final String BODY_TRACKING_PERMISSION = "com.oculus.permission.BODY_TRACKING";

    private static final int ROOT_BG = 0xFF0A0F11;
    private static final int INK = 0xFF0E1416;
    private static final int RAISED = 0xFF1C2629;
    private static final int LINE = 0xFF2A363A;
    private static final int BORDER = 0xFF2B373A;
    private static final int TEXT = 0xFFE8EDEC;
    private static final int MUTED = 0xFF93A2A3;
    private static final int DIM = 0xFF82918F;
    private static final int AMBER = 0xFFF2A65A;
    private static final int AMBER_INK = 0xFF1A1206;
    private static final int GREEN_INK = 0xFFA8E8C3;
    private static final int RED = 0xFFE4736A;
    private static final int COOL = 0xFF6FC9E8;
    private static final int COOL_INK = 0xFFAEE3F5;
    private static final int SOFT = 0xFFC8D4D2;
    private static final int TRACK_OFF = 0xFF33403F;
    private static final int KNOB_OFF = 0xFF8A999B;
    private static final int RADIO_OFF = 0xFF4A5A5D;

    private static final class Pc {
        final String name;
        final String host;
        final boolean manual;

        Pc(String name, String host, boolean manual) {
            this.name = name;
            this.host = host;
            this.manual = manual;
        }
    }

    private final List<Pc> pcs = new ArrayList<>();
    private final Set<String> knownHosts = new HashSet<>();
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private LinearLayout pcList;
    private TextView networkHint;
    private TextView status;
    private TextView updateTitle;
    private TextView updateAction;
    private LinearLayout updateCard;
    private EditText hostInput;
    private Toggle musicToggle;
    private String selectedHost;
    private String updateUrl;
    private NsdManager nsdManager;
    private NsdManager.DiscoveryListener discoveryListener;
    private WifiManager.MulticastLock discoveryMulticastLock;
    private TextView findButton;
    private String pendingHost;
    private String installedVersion = "";

    private final Runnable discoveryTimeout = () -> {
        if (discoveryListener != null && pcs.isEmpty()) {
            networkHint.setText("No KitVR PC answered. Add one by address below.");
            stopDiscovery();
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        nsdManager = (NsdManager) getSystemService(NSD_SERVICE);
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            installedVersion = info.versionName == null ? "" : info.versionName;
        } catch (PackageManager.NameNotFoundException ignored) {
        }
        loadSavedPcs();
        buildUi();
        renderPcs();
        checkForUpdate();
    }

    @Override
    protected void onDestroy() {
        stopDiscovery();
        worker.shutdownNow();
        super.onDestroy();
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(ROOT_BG);
        root.setPadding(dp(24), dp(24), dp(24), dp(24));

        ScrollView scroll = new ScrollView(this);
        scroll.setClipToPadding(false);

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(18), dp(18), dp(18), dp(18));
        panel.setBackground(box(INK, BORDER, 14, 1));
        scroll.addView(panel, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(
                dp(560), ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.addView(new MarkView(this), new LinearLayout.LayoutParams(dp(32), dp(32)));

        LinearLayout brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.VERTICAL);
        brand.setPadding(dp(12), 0, 0, 0);
        brand.addView(text("KitVR", 17, TEXT, true));
        brand.addView(text("PC to Quest streaming", 11.5f, MUTED, false));
        header.addView(brand, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        panel.addView(header);
        panel.addView(divider(), margins(ViewGroup.LayoutParams.MATCH_PARENT, 1, 14, 0));

        LinearLayout networkHead = sectionHeader("On your network");
        findButton = text("Find PCs", 11.5f, COOL, true);
        findButton.setPadding(dp(10), dp(6), dp(2), dp(6));
        findButton.setOnClickListener(v -> discoverServers());
        networkHead.addView(findButton);
        panel.addView(networkHead, margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 18, 10));

        pcList = new LinearLayout(this);
        pcList.setOrientation(LinearLayout.VERTICAL);
        panel.addView(pcList, margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 0, 0));

        networkHint = text("", 11.5f, DIM, false);
        networkHint.setVisibility(View.GONE);
        panel.addView(networkHint, margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 8, 0));

        panel.addView(sectionHeader("Add a PC"), margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 18, 10));

        LinearLayout manual = new LinearLayout(this);
        manual.setOrientation(LinearLayout.HORIZONTAL);
        manual.setGravity(Gravity.CENTER_VERTICAL);

        hostInput = new EditText(this);
        hostInput.setSingleLine(true);
        hostInput.setHint("192.168.1.50");
        hostInput.setText(getPreferences(MODE_PRIVATE).getString("host", ""));
        hostInput.setTextColor(TEXT);
        hostInput.setHintTextColor(DIM);
        hostInput.setTextSize(13.5f);
        hostInput.setTypeface(Typeface.MONOSPACE);
        hostInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        hostInput.setPadding(dp(12), 0, dp(12), 0);
        hostInput.setBackground(box(RAISED, LINE, 9, 1));
        hostInput.setImeOptions(EditorInfo.IME_ACTION_DONE);
        hostInput.setOnEditorActionListener((v, actionId, event) -> {
            addManualPc();
            return true;
        });
        manual.addView(hostInput, new LinearLayout.LayoutParams(
                0, dp(44), 1f));

        TextView addButton = action("Add");
        addButton.setOnClickListener(v -> addManualPc());
        LinearLayout.LayoutParams addParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        addParams.leftMargin = dp(8);
        manual.addView(addButton, addParams);
        panel.addView(manual);

        panel.addView(sectionHeader("Options"), margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 18, 10));

        LinearLayout optionRow = new LinearLayout(this);
        optionRow.setOrientation(LinearLayout.HORIZONTAL);
        optionRow.setGravity(Gravity.CENTER_VERTICAL);
        optionRow.setPadding(dp(14), dp(13), dp(14), dp(13));
        optionRow.setBackground(box(RAISED, LINE, 10, 1));
        LinearLayout optionText = new LinearLayout(this);
        optionText.setOrientation(LinearLayout.VERTICAL);
        optionText.addView(text("Music while connecting", 14, TEXT, true));
        optionText.addView(text("Quiet loop until the first frame arrives", 11.5f, MUTED, false));
        optionRow.addView(optionText, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        musicToggle = new Toggle(this);
        musicToggle.setChecked(getPreferences(MODE_PRIVATE).getBoolean("waiting_music", true));
        musicToggle.setListener(checked ->
                getPreferences(MODE_PRIVATE).edit().putBoolean("waiting_music", checked).apply());
        optionRow.addView(musicToggle, new LinearLayout.LayoutParams(dp(40), dp(22)));
        panel.addView(optionRow);

        updateCard = new LinearLayout(this);
        updateCard.setOrientation(LinearLayout.HORIZONTAL);
        updateCard.setGravity(Gravity.CENTER_VERTICAL);
        updateCard.setPadding(dp(14), dp(13), dp(14), dp(13));
        LinearLayout updateText = new LinearLayout(this);
        updateText.setOrientation(LinearLayout.VERTICAL);
        updateTitle = text("Checking for updates...", 13.5f, TEXT, true);
        TextView updateSub = text(installedVersion.isEmpty()
                ? "KitVR Quest" : "Installed " + installedVersion, 11.5f, MUTED, false);
        updateText.addView(updateTitle);
        updateText.addView(updateSub);
        updateCard.addView(updateText, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        updateAction = text("Download", 12.5f, COOL_INK, true);
        updateAction.setGravity(Gravity.CENTER);
        updateAction.setPadding(dp(14), dp(11), dp(14), dp(11));
        updateAction.setBackground(box(0x1F6FC9E8, 0x666FC9E8, 9, 1));
        updateAction.setVisibility(View.GONE);
        updateAction.setOnClickListener(v -> downloadAndInstall());
        updateCard.addView(updateAction);
        styleUpdateCard(false);
        panel.addView(updateCard, margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 18, 0));

        status = text("Pick a PC on your network, or add one by address.", 11.5f, DIM, false);
        panel.addView(status, margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 16, 0));

        LinearLayout cta = new LinearLayout(this);
        cta.setOrientation(LinearLayout.HORIZONTAL);
        cta.setGravity(Gravity.CENTER);
        cta.setBackground(box(AMBER, AMBER, 12, 0));
        cta.addView(new ArrowView(this), new LinearLayout.LayoutParams(dp(15), dp(15)));
        TextView ctaLabel = text("Connect", 16, AMBER_INK, true);
        ctaLabel.setPadding(dp(9), 0, 0, 0);
        cta.addView(ctaLabel);
        cta.setOnClickListener(v -> connect());
        panel.addView(cta, margins(ViewGroup.LayoutParams.MATCH_PARENT, dp(54), 14, 0));

        LinearLayout footer = new LinearLayout(this);
        footer.setOrientation(LinearLayout.HORIZONTAL);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        TextView ports = text("TCP 9943 / UDP 9944", 10.5f, DIM, false);
        ports.setTypeface(Typeface.MONOSPACE);
        TextView license = text("MIT licensed", 10.5f, DIM, false);
        license.setTypeface(Typeface.MONOSPACE);
        footer.addView(ports, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        footer.addView(license);
        panel.addView(footer, margins(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, 12, 0));
    }

    private void loadSavedPcs() {
        String saved = getPreferences(MODE_PRIVATE).getString("manual_pcs", "");
        if (saved == null || saved.isEmpty()) {
            String last = getPreferences(MODE_PRIVATE).getString("host", "");
            if (last != null && !last.isEmpty()) {
                pcs.add(new Pc(last, last, true));
                knownHosts.add(last);
                selectedHost = last;
            }
            return;
        }
        try {
            JSONArray array = new JSONArray(saved);
            for (int i = 0; i < array.length(); i++) {
                JSONObject entry = array.getJSONObject(i);
                String host = entry.optString("host", "");
                if (host.isEmpty() || !knownHosts.add(host)) continue;
                pcs.add(new Pc(entry.optString("name", host), host, true));
            }
        } catch (Exception ignored) {
        }
        String last = getPreferences(MODE_PRIVATE).getString("host", "");
        if (last != null && !last.isEmpty() && knownHosts.contains(last)) selectedHost = last;
    }

    private void saveSavedPcs() {
        JSONArray array = new JSONArray();
        for (Pc pc : pcs) {
            if (!pc.manual) continue;
            try {
                JSONObject entry = new JSONObject();
                entry.put("name", pc.name);
                entry.put("host", pc.host);
                array.put(entry);
            } catch (Exception ignored) {
            }
        }
        getPreferences(MODE_PRIVATE).edit().putString("manual_pcs", array.toString()).apply();
    }

    private void addManualPc() {
        String host = hostInput.getText().toString().trim();
        if (host.isEmpty()) {
            hostInput.setError("Enter your PC's IP address");
            return;
        }
        host = host.replaceFirst("^[a-zA-Z]+://", "");
        int slash = host.indexOf('/');
        if (slash > 0) host = host.substring(0, slash);
        if (knownHosts.contains(host)) {
            selectedHost = host;
            renderPcs();
            setStatus("Already in your list.", false);
            return;
        }
        pcs.add(new Pc(host, host, true));
        knownHosts.add(host);
        selectedHost = host;
        saveSavedPcs();
        hostInput.setText("");
        hostInput.clearFocus();
        ((InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE))
                .hideSoftInputFromWindow(hostInput.getWindowToken(), 0);
        renderPcs();
        setStatus("Added " + host + ".", false);
    }

    private void renderPcs() {
        if (pcList == null) return;
        pcList.removeAllViews();
        if (pcs.isEmpty()) {
            networkHint.setText("No PCs yet. Tap Find PCs, or add one by address below.");
            networkHint.setVisibility(View.VISIBLE);
            return;
        }
        networkHint.setVisibility(View.GONE);
        for (Pc pc : pcs) {
            pcList.addView(pcRow(pc), margins(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 0, 8));
        }
    }

    private View pcRow(Pc pc) {
        boolean selected = pc.host.equals(selectedHost);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(13), dp(14), dp(13));
        row.setBackground(selected
                ? box(0x14F2A65A, 0x80F2A65A, 10, 1)
                : box(RAISED, LINE, 10, 1));

        View radio = new View(this);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(selected ? AMBER : 0x00000000);
        dot.setStroke(dp(2), selected ? AMBER : RADIO_OFF);
        radio.setBackground(dot);
        row.addView(radio, new LinearLayout.LayoutParams(dp(18), dp(18)));

        LinearLayout nameBlock = new LinearLayout(this);
        nameBlock.setOrientation(LinearLayout.VERTICAL);
        nameBlock.setPadding(dp(12), 0, dp(8), 0);
        nameBlock.addView(text(pc.name, 15.5f, TEXT, true));
        TextView ip = text(pc.host, 11.5f, MUTED, false);
        ip.setTypeface(Typeface.MONOSPACE);
        nameBlock.addView(ip);
        row.addView(nameBlock, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        row.addView(pc.manual ? badge("Manual", false) : badge("Found", true));

        row.setOnClickListener(v -> {
            selectedHost = pc.host;
            getPreferences(MODE_PRIVATE).edit().putString("host", pc.host).apply();
            renderPcs();
            setStatus("Selected " + pc.name + ".", false);
        });
        if (pc.manual) {
            row.setOnLongClickListener(v -> {
                pcs.remove(pc);
                knownHosts.remove(pc.host);
                if (pc.host.equals(selectedHost)) selectedHost = null;
                saveSavedPcs();
                renderPcs();
                Toast.makeText(this, "Removed " + pc.name, Toast.LENGTH_SHORT).show();
                return true;
            });
        }
        return row;
    }

    private void connect() {
        if (selectedHost == null || selectedHost.isEmpty()) {
            setStatus("Pick a PC first, or add one by address.", true);
            return;
        }
        String host = selectedHost;
        getPreferences(MODE_PRIVATE).edit().putString("host", host).apply();
        ((InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE))
                .hideSoftInputFromWindow(hostInput.getWindowToken(), 0);
        ArrayList<String> missingPermissions = new ArrayList<>();
        if (checkSelfPermission(HAND_TRACKING_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            missingPermissions.add(HAND_TRACKING_PERMISSION);
        }
        if (checkSelfPermission(BODY_TRACKING_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            missingPermissions.add(BODY_TRACKING_PERMISSION);
        }
        if (!missingPermissions.isEmpty()) {
            pendingHost = host;
            setStatus("Allow optional hand and body tracking, or deny and keep using controllers.",
                    false);
            requestPermissions(missingPermissions.toArray(new String[0]), REQUEST_TRACKING_PERMISSIONS);
            return;
        }
        launchNative(host);
    }

    private void launchNative(String host) {
        setStatus("Starting KitVR. Make sure the server is running on " + host + ".", false);
        Intent intent = new Intent(this, KitVrActivity.class);
        intent.putExtra("host", host);
        intent.putExtra("waiting_music", getPreferences(MODE_PRIVATE).getBoolean("waiting_music", true));
        startActivity(intent);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_TRACKING_PERMISSIONS && pendingHost != null) {
            String host = pendingHost;
            pendingHost = null;
            launchNative(host);
        }
    }

    private void discoverServers() {
        if (discoveryListener != null) return;
        if (nsdManager == null) {
            networkHint.setText("Network discovery is unavailable. Add your PC by address.");
            networkHint.setVisibility(View.VISIBLE);
            return;
        }
        WifiManager wifiManager = (WifiManager) getApplicationContext()
                .getSystemService(WIFI_SERVICE);
        if (wifiManager != null) {
            discoveryMulticastLock = wifiManager.createMulticastLock("KitVRDiscovery");
            discoveryMulticastLock.setReferenceCounted(false);
            discoveryMulticastLock.acquire();
        }
        findButton.setEnabled(false);
        findButton.setText("Searching...");
        networkHint.setText("Searching your network...");
        networkHint.setVisibility(View.VISIBLE);
        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onDiscoveryStarted(String serviceType) {
            }

            @Override
            public void onServiceFound(NsdServiceInfo service) {
                if (!isKitVrServiceType(service.getServiceType())) return;
                nsdManager.resolveService(service, new NsdManager.ResolveListener() {
                    @Override
                    public void onResolveFailed(NsdServiceInfo info, int errorCode) {
                    }

                    @Override
                    public void onServiceResolved(NsdServiceInfo info) {
                        Inet4Address address = findIpv4Address(info);
                        if (address == null) return;
                        String host = address.getHostAddress();
                        if (host == null) return;
                        String name = info.getServiceName();
                        final String label = (name == null || name.isEmpty()) ? host : name;
                        runOnUiThread(() -> {
                            if (!knownHosts.add(host)) return;
                            Pc pc = new Pc(label, host, false);
                            pcs.add(pc);
                            if (selectedHost == null) selectedHost = pc.host;
                            renderPcs();
                        });
                    }
                });
            }

            @Override
            public void onServiceLost(NsdServiceInfo service) {
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
            }

            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                runOnUiThread(() -> {
                    networkHint.setText("Discovery unavailable. Add your PC by address.");
                    networkHint.setVisibility(View.VISIBLE);
                    stopDiscovery();
                });
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                runOnUiThread(() -> stopDiscovery());
            }
        };
        nsdManager.discoverServices("_kitvr._tcp.", NsdManager.PROTOCOL_DNS_SD, discoveryListener);
        mainHandler.postDelayed(discoveryTimeout, 5000);
    }

    private void stopDiscovery() {
        mainHandler.removeCallbacks(discoveryTimeout);
        if (discoveryListener != null) {
            try {
                nsdManager.stopServiceDiscovery(discoveryListener);
            } catch (IllegalArgumentException ignored) {
            }
            discoveryListener = null;
        }
        if (discoveryMulticastLock != null) {
            if (discoveryMulticastLock.isHeld()) discoveryMulticastLock.release();
            discoveryMulticastLock = null;
        }
        if (findButton != null) {
            findButton.setEnabled(true);
            findButton.setText("Find PCs");
        }
    }

    private static boolean isKitVrServiceType(String serviceType) {
        if (serviceType == null) return false;
        String normalized = serviceType.toLowerCase(Locale.ROOT);
        while (normalized.endsWith(".")) {
            normalized = normalized.substring(0, normalized.length() - 1);
        }
        if (normalized.endsWith(".local")) {
            normalized = normalized.substring(0, normalized.length() - 6);
        }
        return normalized.equals("_kitvr._tcp");
    }

    private static Inet4Address findIpv4Address(NsdServiceInfo service) {
        if (Build.VERSION.SDK_INT >= 34) {
            for (InetAddress address : service.getHostAddresses()) {
                if (address instanceof Inet4Address) return (Inet4Address) address;
            }
        }
        InetAddress address = service.getHost();
        return address instanceof Inet4Address ? (Inet4Address) address : null;
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
                        updateTitle.setText("Version " + latest + " is available");
                        updateAction.setVisibility(View.VISIBLE);
                        styleUpdateCard(true);
                    });
                } else {
                    runOnUiThread(() -> {
                        updateTitle.setText("You're up to date (" + current + ").");
                        updateAction.setVisibility(View.GONE);
                        styleUpdateCard(false);
                    });
                }
            } catch (Exception e) {
                runOnUiThread(() -> {
                    updateTitle.setText("Couldn't check for updates.");
                    updateAction.setVisibility(View.GONE);
                    styleUpdateCard(false);
                });
            } finally {
                if (connection != null) connection.disconnect();
            }
        });
    }

    private void downloadAndInstall() {
        if (updateUrl == null) return;
        updateAction.setEnabled(false);
        updateAction.setText("Downloading");
        updateTitle.setText("Downloading update...");
        worker.execute(() -> {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(updateUrl).openConnection();
                connection.setConnectTimeout(15000);
                connection.setReadTimeout(30000);
                File directory = new File(getCacheDir(), "updates");
                if (!directory.exists() && !directory.mkdirs()) {
                    throw new IllegalStateException("Cannot create update folder");
                }
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
                    updateTitle.setText("Download failed. Try again when online.");
                    updateAction.setEnabled(true);
                    updateAction.setText("Download");
                });
            } finally {
                if (connection != null) connection.disconnect();
            }
        });
    }

    private void installApk(File apk) {
        if (android.os.Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
            updateTitle.setText("Allow KitVR to install updates, then tap Download again.");
            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + getPackageName())));
            updateAction.setEnabled(true);
            updateAction.setText("Download");
            return;
        }
        Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".updates", apk);
        Intent install = new Intent(Intent.ACTION_VIEW);
        install.setDataAndType(uri, "application/vnd.android.package-archive");
        install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(install);
        updateTitle.setText("Android will ask you to confirm the update.");
        updateAction.setEnabled(true);
        updateAction.setText("Download");
    }

    private void styleUpdateCard(boolean highlight) {
        if (updateCard == null) return;
        updateCard.setBackground(highlight
                ? box(0x126FC9E8, 0x596FC9E8, 10, 1)
                : box(RAISED, LINE, 10, 1));
    }

    private void setStatus(String message, boolean error) {
        if (status == null) return;
        status.setText(message);
        status.setTextColor(error ? RED : DIM);
    }

    private LinearLayout sectionHeader(String title) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView label = text(title.toUpperCase(Locale.US), 10.5f, MUTED, true);
        label.setLetterSpacing(0.13f);
        row.addView(label, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    private TextView text(String value, float size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) view.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        return view;
    }

    private TextView action(String label) {
        TextView view = text(label, 12.5f, SOFT, true);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(16), 0, dp(16), 0);
        view.setBackground(box(RAISED, LINE, 9, 1));
        return view;
    }

    private TextView badge(String label, boolean good) {
        TextView view = text(label.toUpperCase(Locale.US), 10.5f, good ? GREEN_INK : MUTED, true);
        view.setLetterSpacing(0.06f);
        view.setPadding(dp(8), dp(4), dp(8), dp(4));
        view.setBackground(good
                ? box(0x2262C98E, 0x5962C98E, 6, 1)
                : box(0x1A93A2A3, 0xFF3A4749, 6, 1));
        return view;
    }

    private View divider() {
        View view = new View(this);
        view.setBackgroundColor(LINE);
        return view;
    }

    private GradientDrawable box(int fill, int stroke, int radius, int strokeWidth) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.RECTANGLE);
        shape.setColor(fill);
        shape.setCornerRadius((float) dp(radius));
        if (strokeWidth > 0) shape.setStroke(dp(strokeWidth), stroke);
        return shape;
    }

    private LinearLayout.LayoutParams margins(int width, int height, int top, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(width, height);
        params.topMargin = dp(top);
        params.bottomMargin = dp(bottom);
        return params;
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
        try {
            return digits.isEmpty() ? 0 : Integer.parseInt(digits);
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }

    private static String readResponse(InputStream input) throws Exception {
        try (InputStream stream = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = stream.read(buffer)) != -1) output.write(buffer, 0, count);
            return output.toString("UTF-8");
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private final class MarkView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path glyph = new Path();

        MarkView(Context context) {
            super(context);
            GradientDrawable tile = new GradientDrawable();
            tile.setColor(AMBER);
            tile.setCornerRadius((float) dp(9));
            setBackground(tile);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth();
            float h = getHeight();
            float s = w / 24f;
            float dx = (w - 24f * s) / 2f;
            float dy = (h - 24f * s) / 2f;
            glyph.reset();
            glyph.moveTo(dx + 4f * s, dy + 19f * s);
            glyph.lineTo(dx + 12f * s, dy + 4f * s);
            glyph.lineTo(dx + 20f * s, dy + 19f * s);
            glyph.lineTo(dx + 12f * s, dy + 15.6f * s);
            glyph.close();
            paint.setColor(AMBER_INK);
            canvas.drawPath(glyph, paint);
        }
    }

    private final class ArrowView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();

        ArrowView(Context context) {
            super(context);
            paint.setColor(AMBER_INK);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2));
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeJoin(Paint.Join.ROUND);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth();
            float h = getHeight();
            float s = w / 24f;
            path.reset();
            path.moveTo(4f * s, 12f * s);
            path.lineTo(19f * s, 12f * s);
            path.moveTo(13f * s, 6f * s);
            path.lineTo(19f * s, 12f * s);
            path.lineTo(13f * s, 18f * s);
            canvas.drawPath(path, paint);
        }
    }

    private interface ToggleListener {
        void onChanged(boolean checked);
    }

    private final class Toggle extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private boolean checked;
        private ToggleListener listener;

        Toggle(Context context) {
            super(context);
            setClickable(true);
            setOnClickListener(v -> {
                checked = !checked;
                invalidate();
                if (listener != null) listener.onChanged(checked);
            });
        }

        void setChecked(boolean value) {
            checked = value;
            invalidate();
        }

        void setListener(ToggleListener value) {
            listener = value;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth();
            float h = getHeight();
            float radius = h / 2f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(checked ? AMBER : TRACK_OFF);
            canvas.drawRoundRect(0f, 0f, w, h, radius, radius, paint);
            float inset = h * 0.136f;
            float knob = h - inset * 2f;
            float left = checked ? w - inset - knob : inset;
            paint.setColor(checked ? AMBER_INK : KNOB_OFF);
            canvas.drawCircle(left + knob / 2f, h / 2f, knob / 2f, paint);
        }
    }
}
