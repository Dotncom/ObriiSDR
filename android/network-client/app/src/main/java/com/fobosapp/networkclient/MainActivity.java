package com.fobosapp.networkclient;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.Locale;

public final class MainActivity extends Activity implements FobosNetworkClient.Listener {
    private static final String PREFS = "fobos_network_client";
    private static final int DEFAULT_PORT = 21090;
    private static final int AUTO_APPLY_DELAY_MS = 650;
    private static final int FAST_APPLY_DELAY_MS = 120;
    private static final int PAN_APPLY_DELAY_MS = 80;
    private static final int PAN_RETUNE_INTERVAL_MS = 120;
    private static final int RELIABLE_SETTINGS_REPEAT_MS = 280;
    private static final int RELIABLE_SETTINGS_REPEAT_COUNT = 2;
    private static final int REMOTE_ECHO_GUARD_MS = 2500;
    private static final int LEVEL_DB_MIN = -160;
    private static final int LEVEL_DB_MAX = 90;
    private static final int LEVEL_MIN_GAP_DB = 5;
    private static final double RF_MIN_CENTER_FREQUENCY = 50_000_000.0;
    private static final double RF_MIN_LISTENING_FREQUENCY = 25_000_000.0;
    private static final double RF_EXPERIMENTAL_MAX_FREQUENCY = 7_750_000_000.0;
    private static final String USB_LOG_TAG = "FobosUsbSandbox";
    private static final int RECEIVER_MODE_NETWORK = 0;
    private static final int RECEIVER_MODE_OTG = 1;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final RadioSettings settings = new RadioSettings();
    private final RadioSettings serverSettings = new RadioSettings();
    private final RxIndicator rxIndicator = new RxIndicator();
    private volatile FobosNetworkClient.SpectrumFrame latestSpectrum;
    private final java.util.concurrent.atomic.AtomicBoolean spectrumQueued =
            new java.util.concurrent.atomic.AtomicBoolean();
    private CheckBox reconnectCheck;
    private CheckBox independentVfoCheck;
    private Spinner remoteVfoSpinner;
    private TextView vfoRangeLabel;
    private final Runnable rxIndicatorTick = new Runnable() {
        @Override public void run() {
            if (spectrumView != null) {
                RxIndicator.State state = rxIndicator.state(SystemClock.uptimeMillis());
                String text = state == RxIndicator.State.RX ? "RX" :
                        state == RxIndicator.State.STALE ? "No data" :
                        state == RxIndicator.State.RETRY ? "Retry" :
                        state == RxIndicator.State.ERROR ? "Error" :
                        state == RxIndicator.State.STOP ? "Stop" :
                        state == RxIndicator.State.WAIT ? "Wait" : "Off";
                if (isIndependentVfo()) text = client.selectedVfo().optString("vfoId", "A") + " " + text;
                int color = state == RxIndicator.State.RX ? 0xff57d68d :
                        state == RxIndicator.State.ERROR ? 0xffff7373 :
                        state == RxIndicator.State.RETRY || state == RxIndicator.State.STALE ||
                                state == RxIndicator.State.WAIT ? 0xffe8c96b : 0xff87949e;
                spectrumView.setRxStatus(text, color);
            }
            mainHandler.postDelayed(this, 1000);
        }
    };
    private boolean hasServerSettings;
    private int serverProcessingMode = RadioSettings.PROCESSING_SERVER_SIDE;
    private final Runnable serverStateRecovery = () -> {
        if (this.client != null && !isOtgMode()) this.client.requestServerState();
    };
    private final Runnable deferredSettingsApply = () -> sendControlNow("settings", false, true);
    private final Runnable deferredUsbPreviewApply = () -> applyUsbOtgPreviewSettings(false);
    private final Runnable deferredPanRetune = () -> {
        lastPanRetuneMs = SystemClock.uptimeMillis();
        applyRetuneSettingsNow(false);
    };
    private final Runnable reliableSettingsRepeat = new Runnable() {
        @Override
        public void run() {
            if (pendingReliableSettingsRepeats <= 0) {
                return;
            }
            if (client == null || !client.isConnected() || !client.canControl()) {
                pendingReliableSettingsRepeats = 0;
                return;
            }
            sendControlNow("settings", false, true);
            --pendingReliableSettingsRepeats;
            if (pendingReliableSettingsRepeats > 0) {
                mainHandler.postDelayed(this, RELIABLE_SETTINGS_REPEAT_MS);
            }
        }
    };
    private FobosNetworkClient client;
    private PcmAudioPlayer audioPlayer;
    private UsbSandbox usbSandbox;

    private EditText hostEdit;
    private EditText portEdit;
    private EditText centerEdit;
    private EditText listenEdit;
    private EditText bandwidthEdit;
    private Spinner sampleRateSpinner;
    private Spinner fftSpinner;
    private Spinner inputSpinner;
    private Spinner modulationSpinner;
    private CheckBox audioCheck;
    private CheckBox bindTuningToCenterCheck;
    private CheckBox suppressServerCheck;
    private CheckBox fullResolutionSpectrumCheck;
    private CheckBox generalBandMarkersCheck;
    private CheckBox amateurBandMarkersCheck;
    private Button receiverModeButton;
    private Button connectButton;
    private Button startButton;
    private Button stopButton;
    private Button settingsButton;
    private Button controlsButton;
    private Button settingsToggleButton;
    private Button requestControlButton;
    private Button usbScanButton;
    private Button usbPermissionButton;
    private Button usbOpenButton;
    private Button usbInfoButton;
    private Button usbReadButton;
    private Button usbCloseButton;
    private Button usbSampleButton;
    private Button usbNativeBenchButton;
    private Button usbToolsButton;
    private Button usbTargetButton;
    private TextView statusText;
    private TextView levelMinLabel;
    private TextView levelMaxLabel;
    private TextView zoomBottomLabel;
    private TextView zoomSideLabel;
    private TextView fineTuneLabel;
    private View fineTuneModeButton;
    private TextView logText;
    private SeekBar levelMinSeek;
    private SeekBar levelMaxSeek;
    private SeekBar zoomBottomSeek;
    private SeekBar zoomSideSeek;
    private FineTuneDialView fineTuneDial;
    private SpectrumView spectrumView;
    private ScrollView controlsScroll;
    private ScrollView settingsScroll;
    private LinearLayout rootLayout;
    private LinearLayout commandPanel;
    private LinearLayout contentPanel;
    private LinearLayout levelPanel;
    private LinearLayout zoomBottomPanel;
    private LinearLayout zoomSidePanel;
    private LinearLayout usbToolsRow;
    private LinearLayout usbSessionRow;
    private LinearLayout usbSampleRow;

    private volatile boolean audioPlaybackEnabled = true;
    private boolean controlsVisible = true;
    private boolean settingsVisible = false;
    private boolean fullResolutionSpectrumFrames = false;
    private int pendingReliableSettingsRepeats = 0;
    private boolean suppressUiCallbacks = false;
    private boolean suppressLevelCallbacks = false;
    private boolean suppressZoomCallbacks = false;
    private long localSettingsGuardUntilMs = 0L;
    private float displayLevelMin = -130.0f;
    private float displayLevelMax = -50.0f;
    private int spectrumZoomProgress = 0;
    private boolean fineTuneHoldMode = false;
    private boolean showGeneralBandMarkers = false;
    private boolean showAmateurBandMarkers = false;
    private boolean usbToolsVisible = false;
    private boolean bindListeningToCenter = false;
    private int usbReceiverTarget = UsbSandbox.USB_TARGET_AUTO;
    private int receiverMode = RECEIVER_MODE_NETWORK;
    private long lastPanRetuneMs = 0L;

    private boolean sampleRateOptionsAreRtl = false;

    private final double[] fobosSampleRates = new double[] {
            8_000_000.0, 10_000_000.0, 12_500_000.0, 16_000_000.0,
            20_000_000.0, 25_000_000.0, 32_000_000.0, 40_000_000.0,
            50_000_000.0, 64_000_000.0, 80_000_000.0
    };
    private final double[] rtlSampleRates = new double[] {
            256_000.0, 300_000.0, 1_024_000.0, 1_400_000.0,
            1_800_000.0, 2_048_000.0, 2_400_000.0, 2_560_000.0,
            2_880_000.0, 3_200_000.0
    };
    private final int[] fftLengths = new int[] {
            2_048, 4_096, 8_192, 16_384, 32_768, 65_536, 131_072,
            262_144, 524_288
    };
    private final int[] modulationIds = new int[] {
            RadioSettings.MOD_AM, RadioSettings.MOD_NFM, RadioSettings.MOD_WFM,
            RadioSettings.MOD_USB, RadioSettings.MOD_LSB, RadioSettings.MOD_DSB,
            RadioSettings.MOD_CW, RadioSettings.MOD_DMR
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        client = new FobosNetworkClient(this);
        audioPlayer = new PcmAudioPlayer();
        buildUi();
        usbSandbox = new UsbSandbox(this, new UsbSandbox.Listener() {
            @Override
            public void onUsbLog(String message) {
                appendUsbLog(message);
                runOnMain(() -> syncUsbReceiverTargetFromActiveSession());
            }

            @Override
            public void onUsbSpectrum(FobosNetworkClient.SpectrumFrame frame) {
                onSpectrum(frame);
            }

            @Override
            public void onUsbAudio(byte[] pcmData) {
                onAudio(pcmData);
            }

            @Override
            public void onUsbTelemetry(String message) {
                MainActivity.this.onUsbTelemetry(message);
            }
        });
        usbSandbox.start();
        appendUsbLogIfNotEmpty(usbSandbox.handleLaunchIntent(getIntent()));
        loadPrefs();
        updateButtons();
        mainHandler.post(rxIndicatorTick);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        savePrefs();
        mainHandler.removeCallbacks(deferredSettingsApply);
        mainHandler.removeCallbacks(reliableSettingsRepeat);
        mainHandler.removeCallbacks(deferredUsbPreviewApply);
        mainHandler.removeCallbacks(deferredPanRetune);
        mainHandler.removeCallbacks(serverStateRecovery);
        mainHandler.removeCallbacks(rxIndicatorTick);
        if (client != null) {
            client.shutdown();
        }
        if (audioPlayer != null) {
            audioPlayer.shutdown();
        }
        if (usbSandbox != null) {
            usbSandbox.stop();
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyResponsiveLayout();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (usbSandbox != null) {
            appendUsbLogIfNotEmpty(usbSandbox.handleLaunchIntent(intent));
        }
    }

    @Override
    public void onStatus(String status) {
        runOnMain(() -> {
            if ("Control channel ready".equals(status)) rxIndicator.connection(true, false);
            statusText.setText(status);
            appendLog(status);
            updateButtons();
        });
    }

    @Override
    public void onDisconnected(String reason) {
        cancelPendingNetworkEdits();
        latestSpectrum = null;
        runOnMain(() -> {
            rxIndicator.connection(false, client.isActive());
            statusText.setText(reason);
            appendLog(reason);
            if (audioPlayer != null) {
                audioPlayer.stop();
            }
            updateButtons();
        });
    }

    @Override
    public void onRole(boolean canControl, String peerId, String peerLabel, String controllerPeerId) {
        runOnMain(() -> {
            String role = canControl ? "controller" : "observer";
            statusText.setText("Connected as " + role);
            appendLog("Role: " + role + (peerLabel.isEmpty() ? "" : " (" + peerLabel + ")"));
            updateButtons();
        });
    }

    @Override
    public void onSpectrum(FobosNetworkClient.SpectrumFrame frame) {
        if (frame.localUsb) rxIndicator.connection(true, false);
        rxIndicator.frame(SystemClock.uptimeMillis());
        latestSpectrum = frame;
        if (spectrumQueued.compareAndSet(false, true)) mainHandler.post(this::renderPendingSpectrum);
    }

    private void renderPendingSpectrum() {
        FobosNetworkClient.SpectrumFrame frame = latestSpectrum;
        spectrumQueued.set(false);
        if (frame == null) return;
            boolean waitingForLocalSettingsEcho =
                    SystemClock.uptimeMillis() < localSettingsGuardUntilMs;
            if (!waitingForLocalSettingsEcho) {
                if (frame.inputMode >= RadioSettings.INPUT_RF &&
                        frame.inputMode <= RadioSettings.INPUT_HF_NOISE_CANCEL) {
                    settings.inputMode = frame.inputMode;
                }
                if (isFinite(frame.centerFrequency)) {
                    settings.centerFrequency = frame.centerFrequency;
                    settings.actualFrequency = frame.centerFrequency;
                }
                if (RadioSettings.isDirectInput(settings.inputMode)) {
                    settings.centerFrequency = 0.0;
                    settings.actualFrequency = 0.0;
                }
                if (isFinite(frame.listeningFrequency)) {
                    settings.listeningFrequency = frame.listeningFrequency;
                }
                if (!isIndependentVfo())
                    settings.sampleRate = frame.sampleRate > 0.0 ? frame.sampleRate : settings.sampleRate;
                if (frame.requestedFftLength > 0) {
                    settings.fftLength = frame.requestedFftLength;
                }
                settings.bandwidth = frame.bandwidth > 0.0 ? frame.bandwidth : settings.bandwidth;
                settings.modulationType = frame.modulationType;
                updateControlsFromSettings(false);
            }
            spectrumView.setAllowNegativeFrequencies(settings.inputMode == RadioSettings.INPUT_HF_COMBINED);
            spectrumView.setSpectrum(
                    frame.frequencies,
                    frame.magnitudes,
                    frame.minFrequency,
                    frame.maxFrequency,
                    settings.listeningFrequency,
                    settings.bandwidth,
                    settings.modulationType);
            applyStoredZoomToSpectrum();
            updateFineTuneControl();
    }

    @Override
    public void onAudio(byte[] pcmData) {
        rxIndicator.frame(SystemClock.uptimeMillis());
        if (audioPlayer != null && audioPlaybackEnabled) {
            audioPlayer.playPcm(pcmData);
        }
    }

    @Override public void onAudioReset() {
        if (audioPlayer != null) audioPlayer.stop();
    }

    private void onUsbTelemetry(String message) {
        String audioStats = audioPlayer != null ? audioPlayer.statsSnapshot() : "";
        String status = audioStats.isEmpty() ? message : message + " | " + audioStats;
        Log.d(USB_LOG_TAG, status);
        runOnMain(() -> {
            if (statusText != null) {
                statusText.setText(status);
            }
        });
    }

    @Override
    public void onControlMessage(JSONObject command) {
        if ("remoteVfo".equals(command.optString("type"))) {
            runOnMain(() -> applyRemoteVfoState(command));
            return;
        }
        String action = command.optString("action");
        if ("serverState".equals(action) || "settingsAck".equals(action)) {
            runOnMain(() -> {
                if (isOtgMode()) return;
                mainHandler.removeCallbacks(serverStateRecovery);
                mainHandler.removeCallbacks(reliableSettingsRepeat);
                pendingReliableSettingsRepeats = 0;
                localSettingsGuardUntilMs = 0;
                settings.updateFromJson(command.optJSONObject("settings"));
                serverSettings.updateFromJson(command.optJSONObject("settings"));
                hasServerSettings = true;
                serverProcessingMode = command.optInt("processingMode", serverProcessingMode);
                if (command.has("receiverRunning")) rxIndicator.receiver(command.optBoolean("receiverRunning"));
                suppressUiCallbacks = true;
                refreshSampleRateOptions(true);
                replaceSpinnerOptions(fftSpinner, fftLengthLabels());
                replaceSpinnerOptions(modulationSpinner, modulationLabels());
                suppressServerCheck.setChecked(command.optBoolean(
                        "serverDisableLocalVisualAudio", suppressServerCheck.isChecked()));
                fullResolutionSpectrumFrames = command.optBoolean(
                        "fullResolutionSpectrumFrames", fullResolutionSpectrumFrames);
                fullResolutionSpectrumCheck.setChecked(fullResolutionSpectrumFrames);
                audioCheck.setChecked(settings.audioEnabled);
                audioPlaybackEnabled = settings.audioEnabled;
                if (Math.abs(settings.centerFrequency - settings.listeningFrequency) > 0.5) {
                    bindListeningToCenter = false;
                    bindTuningToCenterCheck.setChecked(false);
                }
                suppressUiCallbacks = false;
                updateControlsFromSettings(true);
                if (!command.optBoolean("ok", true))
                    appendLog("Server rejected settings: " + command.optString("reason", "Unknown reason"));
                updateButtons();
            });
        } else if ("priorityRequest".equals(action)) {
            runOnMain(() -> showPriorityRequest(command));
        } else if ("priorityDenied".equals(action) || "controlRejected".equals(action)) {
            runOnMain(() -> appendLog(command.optString("reason", "Control request denied")));
        }
    }

    private void buildUi() {
        rootLayout = new LinearLayout(this);
        rootLayout.setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(
                        android.view.WindowInsets.Type.systemBars() |
                                android.view.WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
            } else {
                view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                        insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            }
            return insets;
        });
        rootLayout.setBackgroundColor(0xff101418);
        commandPanel = new LinearLayout(this);
        commandPanel.setGravity(Gravity.CENTER);
        commandPanel.setBackgroundColor(0xff101418);
        contentPanel = new LinearLayout(this);
        contentPanel.setOrientation(LinearLayout.VERTICAL);
        contentPanel.setBackgroundColor(0xff101418);

        receiverModeButton = button("Net");
        connectButton = button("Connect");
        startButton = button("Start");
        stopButton = button("Stop");
        settingsButton = button("Apply");
        controlsButton = button("Hide");
        settingsToggleButton = button("Settings");
        receiverModeButton.setOnClickListener(v -> toggleReceiverMode());
        connectButton.setOnClickListener(v -> {
            if (isOtgMode()) {
                openUsbOtgSession();
            } else {
                toggleConnection();
            }
        });
        startButton.setOnClickListener(v -> startActiveReceiver());
        stopButton.setOnClickListener(v -> stopActiveReceiver());
        settingsButton.setOnClickListener(v -> applyActiveReceiverSettings(true));
        controlsButton.setOnClickListener(v -> setControlsVisible(!controlsVisible));
        settingsToggleButton.setOnClickListener(v -> setSettingsVisible(!settingsVisible));

        statusText = label("Network disabled");
        statusText.setTextColor(0xffe7edf2);
        statusText.setPadding(dp(10), dp(5), dp(10), dp(3));
        statusText.setTextSize(12.0f);
        levelPanel = buildLevelPanel();

        controlsScroll = new ScrollView(this);
        controlsScroll.setFillViewport(false);
        controlsScroll.setBackgroundColor(0xff182129);
        LinearLayout controlsPanel = new LinearLayout(this);
        controlsPanel.setOrientation(LinearLayout.VERTICAL);
        controlsPanel.setPadding(dp(8), dp(5), dp(8), dp(6));
        controlsScroll.addView(controlsPanel);

        settingsScroll = new ScrollView(this);
        settingsScroll.setFillViewport(false);
        settingsScroll.setBackgroundColor(0xff182129);
        settingsScroll.setVisibility(View.GONE);
        LinearLayout settingsPanel = new LinearLayout(this);
        settingsPanel.setOrientation(LinearLayout.VERTICAL);
        settingsPanel.setPadding(dp(8), dp(5), dp(8), dp(6));
        settingsScroll.addView(settingsPanel);

        LinearLayout connectionRow = row();
        LinearLayout connectionActionRow = row();
        hostEdit = edit("Server IP");
        hostEdit.setInputType(InputType.TYPE_CLASS_TEXT |
                InputType.TYPE_TEXT_VARIATION_URI |
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        portEdit = edit("Port");
        portEdit.setInputType(InputType.TYPE_CLASS_NUMBER);
        requestControlButton = button("Request control");
        requestControlButton.setOnClickListener(v -> sendCommand("requestPriority", true));
        connectionRow.addView(fieldWithLabel("Server IP", hostEdit), new LinearLayout.LayoutParams(0, dp(62), 1.5f));
        connectionRow.addView(fieldWithLabel("Port", portEdit), new LinearLayout.LayoutParams(0, dp(62), 0.7f));
        connectionActionRow.addView(connectButton, new LinearLayout.LayoutParams(0, dp(44), 0.8f));
        connectionActionRow.addView(requestControlButton, new LinearLayout.LayoutParams(0, dp(44), 1.2f));

        LinearLayout networkOptions = row();
        reconnectCheck = new CheckBox(this);
        reconnectCheck.setText("Auto reconnect");
        reconnectCheck.setTextColor(0xffe7edf2);
        reconnectCheck.setChecked(true);
        reconnectCheck.setOnCheckedChangeListener((view, checked) -> {
            if (!suppressUiCallbacks) { client.setAutoReconnect(checked); updateButtons(); }
        });
        independentVfoCheck = new CheckBox(this);
        independentVfoCheck.setText("Independent VFO");
        independentVfoCheck.setTextColor(0xffe7edf2);
        independentVfoCheck.setOnCheckedChangeListener((view, checked) -> {
            if (suppressUiCallbacks) return;
            client.setRemoteVfoMode(checked);
            cancelPendingNetworkEdits();
            latestSpectrum = null;
            localSettingsGuardUntilMs = 0;
            suppressUiCallbacks = true;
            replaceSpinnerOptions(modulationSpinner, modulationLabels());
            replaceSpinnerOptions(fftSpinner, fftLengthLabels());
            suppressUiCallbacks = false;
            updateControlsFromSettings(true);
            updateButtons();
        });
        remoteVfoSpinner = spinner(new String[]{"A", "B"});
        remoteVfoSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            public void onNothingSelected(AdapterView<?> parent) { }
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (suppressUiCallbacks || !isIndependentVfo()) return;
                mainHandler.removeCallbacks(deferredSettingsApply);
                mainHandler.removeCallbacks(reliableSettingsRepeat);
                pendingReliableSettingsRepeats = 0;
                localSettingsGuardUntilMs = 0;
                client.selectVfo(position == 0 ? "A" : "B");
                latestSpectrum = null;
                rxIndicator.connection(client.isConnected(), false);
                applyRemoteVfoState(null);
            }
        });
        networkOptions.addView(reconnectCheck, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        networkOptions.addView(independentVfoCheck, new LinearLayout.LayoutParams(0, dp(44), 1.2f));
        networkOptions.addView(remoteVfoSpinner, new LinearLayout.LayoutParams(0, dp(44), 0.8f));
        vfoRangeLabel = label("");
        vfoRangeLabel.setTextSize(11);

        LinearLayout usbPrimaryRow = row();
        usbToolsRow = row();
        usbSessionRow = row();
        usbSampleRow = row();
        usbScanButton = button("USB scan");
        usbPermissionButton = button("USB permission/open");
        usbOpenButton = button("OTG open");
        usbInfoButton = button("OTG info");
        usbReadButton = button("OTG read");
        usbCloseButton = button("OTG close");
        usbSampleButton = button("OTG sample test");
        usbNativeBenchButton = button("Native bench");
        usbToolsButton = button("Tools");
        usbTargetButton = button("USB Auto");
        usbScanButton.setOnClickListener(v -> scanUsbDevices());
        usbPermissionButton.setOnClickListener(v -> requestUsbPermission());
        usbOpenButton.setOnClickListener(v -> openUsbOtgSession());
        usbInfoButton.setOnClickListener(v -> readUsbOtgInfo());
        usbReadButton.setOnClickListener(v -> readUsbOtgProbe());
        usbCloseButton.setOnClickListener(v -> closeUsbOtgSession());
        usbSampleButton.setOnClickListener(v -> runUsbOtgSampleTest());
        usbNativeBenchButton.setOnClickListener(v -> runUsbOtgNativeBenchmark());
        usbToolsButton.setOnClickListener(v -> toggleUsbTools());
        usbTargetButton.setOnClickListener(v -> cycleUsbReceiverTarget());
        usbPrimaryRow.addView(usbTargetButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbPrimaryRow.addView(usbInfoButton, new LinearLayout.LayoutParams(0, dp(44), 0.8f));
        usbPrimaryRow.addView(usbToolsButton, new LinearLayout.LayoutParams(0, dp(44), 0.8f));
        usbToolsRow.addView(usbScanButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbToolsRow.addView(usbPermissionButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbSessionRow.addView(usbOpenButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbSessionRow.addView(usbReadButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbSessionRow.addView(usbCloseButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbSampleRow.addView(usbSampleButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        usbSampleRow.addView(usbNativeBenchButton, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        updateUsbToolsVisibility();

        LinearLayout freqRow = row();
        centerEdit = edit("Center MHz");
        listenEdit = edit("Listen MHz");
        bandwidthEdit = edit("BW kHz");
        addAutoApplyTextWatcher(centerEdit);
        addAutoApplyTextWatcher(listenEdit);
        addAutoApplyTextWatcher(bandwidthEdit);
        freqRow.addView(fieldWithLabel("Center MHz", centerEdit), new LinearLayout.LayoutParams(0, dp(62), 1.0f));
        freqRow.addView(fieldWithLabel("Listen MHz", listenEdit), new LinearLayout.LayoutParams(0, dp(62), 1.0f));
        freqRow.addView(fieldWithLabel("BW kHz", bandwidthEdit), new LinearLayout.LayoutParams(0, dp(62), 0.8f));

        LinearLayout modeRow = row();
        sampleRateSpinner = spinner(sampleRateLabels());
        inputSpinner = spinner(new String[] {"RF", "HF1 + HF2", "HF1", "HF2", "HF1 - HF2 cancel lab"});
        modulationSpinner = spinner(modulationLabels());
        sampleRateSpinner.setOnItemSelectedListener(autoApplySpinnerListener(false));
        inputSpinner.setOnItemSelectedListener(inputModeSpinnerListener());
        modulationSpinner.setOnItemSelectedListener(autoApplySpinnerListener(true));
        modeRow.addView(fieldWithLabel("Sample", sampleRateSpinner), new LinearLayout.LayoutParams(0, dp(62), 1.0f));
        modeRow.addView(fieldWithLabel("Input", inputSpinner), new LinearLayout.LayoutParams(0, dp(62), 0.8f));
        modeRow.addView(fieldWithLabel("Mode", modulationSpinner), new LinearLayout.LayoutParams(0, dp(62), 0.8f));

        LinearLayout fftRow = row();
        fftSpinner = spinner(fftLengthLabels());
        fftSpinner.setOnItemSelectedListener(autoApplySpinnerListener(false));
        fullResolutionSpectrumCheck = new CheckBox(this);
        fullResolutionSpectrumCheck.setText("Full FFT frames");
        fullResolutionSpectrumCheck.setTextColor(0xffe7edf2);
        fullResolutionSpectrumCheck.setChecked(fullResolutionSpectrumFrames);
        fullResolutionSpectrumCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            fullResolutionSpectrumFrames = isChecked;
            sendReliableSettingsNow();
        });
        fftRow.addView(fieldWithLabel("FFT", fftSpinner), new LinearLayout.LayoutParams(0, dp(62), 0.9f));
        fftRow.addView(fullResolutionSpectrumCheck, new LinearLayout.LayoutParams(0, dp(44), 1.1f));

        LinearLayout optionsRow = row();
        audioCheck = new CheckBox(this);
        audioCheck.setText("Audio");
        audioCheck.setTextColor(0xffe7edf2);
        audioCheck.setChecked(true);
        audioCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            audioPlaybackEnabled = isChecked;
            if (!isChecked && audioPlayer != null) {
                audioPlayer.stop();
            }
            scheduleSettingsApply(FAST_APPLY_DELAY_MS);
        });
        suppressServerCheck = new CheckBox(this);
        suppressServerCheck.setText("Disable server local output");
        suppressServerCheck.setTextColor(0xffe7edf2);
        suppressServerCheck.setChecked(true);
        suppressServerCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (!suppressUiCallbacks && !isOtgMode() && client != null && client.isConnected()) {
                client.sendControl("setServerLocalOutput", settings, isChecked,
                        fullResolutionSpectrumFrames, null);
            }
        });
        bindTuningToCenterCheck = new CheckBox(this);
        bindTuningToCenterCheck.setText("Center=tune");
        bindTuningToCenterCheck.setTextColor(0xffe7edf2);
        bindTuningToCenterCheck.setChecked(bindListeningToCenter);
        bindTuningToCenterCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (suppressUiCallbacks) {
                bindListeningToCenter = isChecked;
                return;
            }
            bindListeningToCenter = isChecked;
            collectSettingsFromUi();
            enforceListeningCenterBinding();
            setFrequencyTextsFromSettings(true);
            if (spectrumView != null) {
                spectrumView.setListeningFrequency(settings.listeningFrequency);
            }
            savePrefs();
            applyRetuneSettingsNow(false);
        });
        Button toneButton = button("Tone");
        toneButton.setOnClickListener(v -> {
            if (audioPlayer != null) {
                audioPlayer.playTestTone();
                statusText.setText("Audio test tone");
            }
        });
        optionsRow.addView(audioCheck, new LinearLayout.LayoutParams(0, dp(44), 0.7f));
        optionsRow.addView(toneButton, new LinearLayout.LayoutParams(0, dp(44), 0.6f));
        optionsRow.addView(bindTuningToCenterCheck, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        optionsRow.addView(suppressServerCheck, new LinearLayout.LayoutParams(0, dp(44), 1.3f));

        LinearLayout markerRow = row();
        generalBandMarkersCheck = new CheckBox(this);
        generalBandMarkersCheck.setText("Bands");
        generalBandMarkersCheck.setTextColor(0xffe7edf2);
        generalBandMarkersCheck.setChecked(showGeneralBandMarkers);
        generalBandMarkersCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (suppressUiCallbacks) {
                return;
            }
            showGeneralBandMarkers = isChecked;
            updateBandMarkerVisibility();
            savePrefs();
        });
        amateurBandMarkersCheck = new CheckBox(this);
        amateurBandMarkersCheck.setText("HAM bands");
        amateurBandMarkersCheck.setTextColor(0xffe7edf2);
        amateurBandMarkersCheck.setChecked(showAmateurBandMarkers);
        amateurBandMarkersCheck.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (suppressUiCallbacks) {
                return;
            }
            showAmateurBandMarkers = isChecked;
            updateBandMarkerVisibility();
            savePrefs();
        });
        markerRow.addView(generalBandMarkersCheck, new LinearLayout.LayoutParams(0, dp(44), 1.0f));
        markerRow.addView(amateurBandMarkersCheck, new LinearLayout.LayoutParams(0, dp(44), 1.0f));

        logText = label("");
        logText.setTextSize(12.0f);
        ScrollView logScroll = new ScrollView(this);
        logScroll.setBackgroundColor(0xff0b1014);
        logScroll.addView(logText);

        controlsPanel.addView(freqRow);
        controlsPanel.addView(modeRow);
        controlsPanel.addView(fftRow);

        settingsPanel.addView(connectionRow);
        settingsPanel.addView(connectionActionRow);
        settingsPanel.addView(networkOptions);
        settingsPanel.addView(vfoRangeLabel);
        settingsPanel.addView(usbPrimaryRow);
        settingsPanel.addView(usbToolsRow);
        settingsPanel.addView(usbSessionRow);
        settingsPanel.addView(usbSampleRow);
        settingsPanel.addView(optionsRow);
        settingsPanel.addView(markerRow);
        settingsPanel.addView(statusText, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(38)));
        settingsPanel.addView(logScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(88)));

        spectrumView = new SpectrumView(this);
        spectrumView.setLevelRange(displayLevelMin, displayLevelMax);
        updateBandMarkerVisibility();
        spectrumView.setTuneRequestListener(frequencyHz -> {
            tuneToFrequency(frequencyHz, true);
        });
        spectrumView.setCenterTuneRequestListener(frequencyHz -> {
            centerToFrequency(frequencyHz, true);
        });
        spectrumView.setPanTuneRequestListener(deltaHz -> applySpectrumPanDelta(deltaHz));
        spectrumView.setZoomProgressListener(progress -> {
            spectrumZoomProgress = Math.max(0, Math.min(100, progress));
            updateZoomControls();
            updateFineTuneControl();
        });
        zoomBottomPanel = buildBottomZoomPanel();
        zoomSidePanel = buildSideZoomPanel();

        contentPanel.addView(levelPanel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52)));
        contentPanel.addView(controlsScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(230)));
        contentPanel.addView(settingsScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(230)));
        contentPanel.addView(spectrumView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));
        contentPanel.addView(zoomBottomPanel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(44)));

        setContentView(rootLayout);
        applyResponsiveLayout();
    }

    private void toggleConnection() {
        if (client.isActive()) {
            client.disconnect();
            cancelPendingNetworkEdits();
            latestSpectrum = null;
            rxIndicator.connection(false, false);
            if (audioPlayer != null) {
                audioPlayer.stop();
            }
            statusText.setText("Network disabled");
            appendLog("Disconnected");
            updateButtons();
            return;
        }
        savePrefs();
        String host = hostEdit.getText().toString().trim();
        int port = safeInt(portEdit.getText().toString(), DEFAULT_PORT);
        client.connect(host.isEmpty() ? "127.0.0.1" : host, port);
        updateButtons();
    }

    private boolean isOtgMode() {
        return receiverMode == RECEIVER_MODE_OTG;
    }

    private boolean isIndependentVfo() {
        return !isOtgMode() && client != null && client.isRemoteVfoMode();
    }

    private void applyRemoteVfoState(JSONObject message) {
        if (!isIndependentVfo()) return;
        if (message != null && "audioReset".equals(message.optString("action"))) return;
        JSONObject caps = client.vfoCapabilities();
        JSONObject vfo = client.selectedVfo();
        settings.sampleRate = caps.optDouble("sampleRate", settings.sampleRate);
        refreshSampleRateOptions(true);
        boolean failed = false;
        if (message != null && ("error".equals(message.optString("action")) ||
                "branchStatus".equals(message.optString("action")))) {
            String error = message.optString("message", message.optString("error"));
            if (!error.isEmpty()) { failed = true; rxIndicator.error(); appendLog("VFO: " + error); }
        }
        if (vfo.has("frequencyHz")) {
            settings.inputMode = RadioSettings.INPUT_RF;
            settings.centerFrequency = vfo.optDouble("frequencyHz");
            settings.actualFrequency = settings.centerFrequency;
            settings.listeningFrequency = settings.centerFrequency;
            settings.bandwidth = vfo.optDouble("bandwidthHz", 12500);
            settings.modulationType = vfo.optInt("modulationType", RadioSettings.MOD_NFM);
            settings.fftLength = vfo.optInt("fftLength", 2048);
            settings.audioEnabled = vfo.optBoolean("audioEnabled", true);
        }
        bindListeningToCenter = false;
        localSettingsGuardUntilMs = 0;
        suppressUiCallbacks = true;
        audioCheck.setChecked(settings.audioEnabled);
        bindTuningToCenterCheck.setChecked(false);
        suppressUiCallbacks = false;
        if (caps.has("receiverRunning") && !failed)
            rxIndicator.receiver(caps.optBoolean("receiverRunning") && vfo.optBoolean("active"));
        vfoRangeLabel.setText(caps.has("minFrequency") ? String.format(Locale.US,
                "Server span %.6f – %.6f MHz · VFO %s", caps.optDouble("minFrequency") / 1e6,
                caps.optDouble("maxFrequency") / 1e6, vfo.optString("vfoId", "A")) : "Waiting for VFO session");
        updateControlsFromSettings(true);
        updateButtons();
    }

    private double minimumRfCenterHz() {
        return isOtgMode() ? RF_MIN_CENTER_FREQUENCY : 1.0;
    }

    private double minimumRfListeningHz() {
        return isOtgMode() ? RF_MIN_LISTENING_FREQUENCY : 1.0;
    }

    private void toggleReceiverMode() {
        if (isOtgMode()) {
            if (isUsbOtgPreviewRunning()) {
                stopUsbOtgPreview();
            }
            receiverMode = RECEIVER_MODE_NETWORK;
            statusText.setText("Network mode");
        } else {
            receiverMode = RECEIVER_MODE_OTG;
            if (audioPlayer != null) {
                audioPlayer.stop();
            }
            statusText.setText("OTG mode");
        }
        suppressUiCallbacks = true;
        refreshSampleRateOptions(true);
        replaceSpinnerOptions(fftSpinner, fftLengthLabels());
        replaceSpinnerOptions(modulationSpinner, modulationLabels());
        suppressUiCallbacks = false;
        updateControlsFromSettings(true);
        if (!isOtgMode() && client != null) client.requestServerState();
        savePrefs();
        updateButtons();
    }

    private void startActiveReceiver() {
        if (isOtgMode()) {
            startUsbOtgPreview();
            setControlsVisible(false);
            updateButtons();
            return;
        }
        if (sendCommand("start", true)) {
            setControlsVisible(false);
        }
    }

    private void stopActiveReceiver() {
        if (audioPlayer != null) {
            audioPlayer.stop();
        }
        if (isOtgMode()) {
            mainHandler.removeCallbacks(deferredUsbPreviewApply);
            stopUsbOtgPreview();
            updateButtons();
            return;
        }
        sendCommand("stop", true);
    }

    private void applyActiveReceiverSettings(boolean logExplicitly) {
        if (isOtgMode()) {
            applyUsbOtgPreviewSettings(logExplicitly);
            return;
        }
        sendCommand("settings", logExplicitly);
    }

    private void scanUsbDevices() {
        Log.d(USB_LOG_TAG, "USB scan button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.scanReport());
    }

    private void requestUsbPermission() {
        Log.d(USB_LOG_TAG, "USB permission/open button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.requestPermissionForBestDevice());
        syncUsbReceiverTargetFromActiveSession();
    }

    private void openUsbOtgSession() {
        Log.d(USB_LOG_TAG, "OTG open button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.openReceiverSession());
        syncUsbReceiverTargetFromActiveSession();
    }

    private void readUsbOtgProbe() {
        Log.d(USB_LOG_TAG, "OTG read button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.readReceiverProbe());
        syncUsbReceiverTargetFromActiveSession();
    }

    private void readUsbOtgInfo() {
        Log.d(USB_LOG_TAG, "OTG info button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.readReceiverInfo());
        syncUsbReceiverTargetFromActiveSession();
    }

    private void closeUsbOtgSession() {
        Log.d(USB_LOG_TAG, "OTG close button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.closeReceiverSession());
        applyUsbReceiverTarget();
    }

    private void runUsbOtgSampleTest() {
        Log.d(USB_LOG_TAG, "OTG sample test button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.runReceiverSampleTest());
    }

    private void runUsbOtgNativeBenchmark() {
        Log.d(USB_LOG_TAG, "OTG native benchmark button clicked");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.runNativeBulkBenchmark());
    }

    private void toggleUsbTools() {
        usbToolsVisible = !usbToolsVisible;
        updateUsbToolsVisibility();
    }

    private void cycleUsbReceiverTarget() {
        if (usbReceiverTarget == UsbSandbox.USB_TARGET_AUTO) {
            usbReceiverTarget = UsbSandbox.USB_TARGET_RTL_SDR;
        } else if (usbReceiverTarget == UsbSandbox.USB_TARGET_RTL_SDR) {
            usbReceiverTarget = UsbSandbox.USB_TARGET_FOBOS;
        } else {
            usbReceiverTarget = UsbSandbox.USB_TARGET_AUTO;
        }
        if (usbSandbox != null) {
            appendUsbLogIfNotEmpty(usbSandbox.closeReceiverSession());
        }
        applyUsbReceiverTarget();
        appendLog("USB target: " + usbReceiverTargetLabel());
        savePrefs();
    }

    private void applyUsbReceiverTarget() {
        if (usbSandbox != null) {
            usbSandbox.setPreferredReceiverTarget(usbReceiverTarget);
        }
        if (usbTargetButton != null) {
            usbTargetButton.setText("USB " + usbReceiverTargetLabel());
        }
        normalizeSettingsForReceiverTarget();
        refreshSampleRateOptions(false);
        updateReceiverTargetControls();
    }

    private void syncUsbReceiverTargetFromActiveSession() {
        if (usbSandbox == null) {
            return;
        }
        int activeTarget = usbSandbox.activeReceiverTarget();
        if (activeTarget != UsbSandbox.USB_TARGET_FOBOS &&
                activeTarget != UsbSandbox.USB_TARGET_RTL_SDR) {
            return;
        }
        boolean rtlTarget = activeTarget == UsbSandbox.USB_TARGET_RTL_SDR;
        boolean targetChanged = usbReceiverTarget != activeTarget;
        if (!targetChanged && sampleRateOptionsAreRtl == rtlTarget) {
            return;
        }
        if (targetChanged) {
            usbReceiverTarget = activeTarget;
            if (usbTargetButton != null) {
                usbTargetButton.setText("USB " + usbReceiverTargetLabel());
            }
        }
        normalizeSettingsForReceiverTarget();
        refreshSampleRateOptions(true);
        updateReceiverTargetControls();
        setFrequencyTextsFromSettings(true);
        if (targetChanged) {
            savePrefs();
        }
    }

    private String usbReceiverTargetLabel() {
        if (usbReceiverTarget == UsbSandbox.USB_TARGET_RTL_SDR) {
            return "RTL";
        }
        if (usbReceiverTarget == UsbSandbox.USB_TARGET_FOBOS) {
            return "Fobos";
        }
        return "Auto";
    }

    private boolean isRtlUsbTarget() {
        return effectiveUsbReceiverTarget() == UsbSandbox.USB_TARGET_RTL_SDR;
    }

    private int effectiveUsbReceiverTarget() {
        if (!isOtgMode()) {
            return UsbSandbox.USB_TARGET_FOBOS;
        }
        if (usbSandbox != null) {
            int activeTarget = usbSandbox.activeReceiverTarget();
            if (activeTarget == UsbSandbox.USB_TARGET_FOBOS ||
                    activeTarget == UsbSandbox.USB_TARGET_RTL_SDR) {
                return activeTarget;
            }
            int detectedTarget = usbSandbox.detectedReceiverTarget();
            if (detectedTarget == UsbSandbox.USB_TARGET_FOBOS ||
                    detectedTarget == UsbSandbox.USB_TARGET_RTL_SDR) {
                return detectedTarget;
            }
        }
        return usbReceiverTarget;
    }

    private void normalizeSettingsForReceiverTarget() {
        if (!isRtlUsbTarget()) {
            return;
        }
        settings.inputMode = RadioSettings.INPUT_RF;
        if (!isFinite(settings.centerFrequency) || settings.centerFrequency < RF_MIN_CENTER_FREQUENCY) {
            settings.centerFrequency = isFinite(settings.listeningFrequency) &&
                    settings.listeningFrequency >= RF_MIN_LISTENING_FREQUENCY
                    ? settings.listeningFrequency
                    : 100_000_000.0;
        }
        settings.actualFrequency = settings.centerFrequency;
        if (!isFinite(settings.listeningFrequency) ||
                settings.listeningFrequency < RF_MIN_LISTENING_FREQUENCY ||
                settings.listeningFrequency > RF_EXPERIMENTAL_MAX_FREQUENCY) {
            settings.listeningFrequency = settings.centerFrequency;
        }
    }

    private void updateReceiverTargetControls() {
        if (inputSpinner == null) {
            return;
        }
        boolean rtl = isRtlUsbTarget();
        boolean previousSuppress = suppressUiCallbacks;
        suppressUiCallbacks = true;
        if (rtl) {
            inputSpinner.setSelection(RadioSettings.INPUT_RF, false);
        }
        inputSpinner.setEnabled(!rtl);
        inputSpinner.setAlpha(rtl ? 0.55f : 1.0f);
        suppressUiCallbacks = previousSuppress;
    }

    private void updateUsbToolsVisibility() {
        int visibility = usbToolsVisible ? View.VISIBLE : View.GONE;
        if (usbToolsRow != null) {
            usbToolsRow.setVisibility(visibility);
        }
        if (usbSessionRow != null) {
            usbSessionRow.setVisibility(visibility);
        }
        if (usbSampleRow != null) {
            usbSampleRow.setVisibility(visibility);
        }
        if (usbToolsButton != null) {
            usbToolsButton.setText(usbToolsVisible ? "Hide" : "Tools");
        }
    }

    private void startUsbOtgPreview() {
        Log.d(USB_LOG_TAG, "OTG preview start requested");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        collectSettingsFromUi();
        appendUsbLog(usbSandbox.startReceiverPreview(
                settings.centerFrequency,
                settings.listeningFrequency,
                settings.sampleRate,
                settings.inputMode,
                settings.bandwidth,
                settings.modulationType,
                settings.audioEnabled,
                settings.lnaGain,
                settings.vgaGain,
                settings.fftLength));
        syncUsbReceiverTargetFromActiveSession();
    }

    private void stopUsbOtgPreview() {
        rxIndicator.receiver(false);
        Log.d(USB_LOG_TAG, "OTG preview stop requested");
        if (usbSandbox == null) {
            appendLog("[USB] sandbox is not ready");
            return;
        }
        appendUsbLog(usbSandbox.stopReceiverPreview());
    }

    private boolean isUsbOtgPreviewRunning() {
        return usbSandbox != null && usbSandbox.isReceiverPreviewRunning();
    }

    private void applyUsbOtgPreviewSettings(boolean logExplicitly) {
        applyUsbOtgPreviewSettings(logExplicitly, true);
    }

    private void applyUsbOtgPreviewSettings(boolean logExplicitly, boolean collectUi) {
        if (!isUsbOtgPreviewRunning()) {
            if (logExplicitly) {
                appendUsbLog("[USB OTG] settings will apply on next Start");
            }
            return;
        }
        localSettingsGuardUntilMs = SystemClock.uptimeMillis() + REMOTE_ECHO_GUARD_MS;
        if (collectUi) {
            collectSettingsFromUi();
        }
        String message = usbSandbox.applyReceiverPreviewSettings(
                settings.centerFrequency,
                settings.listeningFrequency,
                settings.sampleRate,
                settings.inputMode,
                settings.bandwidth,
                settings.modulationType,
                settings.audioEnabled,
                settings.lnaGain,
                settings.vgaGain,
                settings.fftLength);
        Log.d(USB_LOG_TAG, String.format(Locale.US,
                "applyUsbOtgPreviewSettings center %.3f listen %.3f sr %.3f input %d mod %d bw %.0f",
                settings.centerFrequency / 1_000_000.0,
                settings.listeningFrequency / 1_000_000.0,
                settings.sampleRate / 1_000_000.0,
                settings.inputMode,
                settings.modulationType,
                settings.bandwidth));
        Log.d(USB_LOG_TAG, "applyUsbOtgPreviewSettings result " + message);
        if (logExplicitly) {
            appendUsbLog(message);
        }
    }

    private boolean sendCommand(String action, boolean logExplicitly) {
        mainHandler.removeCallbacks(deferredSettingsApply);
        mainHandler.removeCallbacks(reliableSettingsRepeat);
        mainHandler.removeCallbacks(deferredUsbPreviewApply);
        mainHandler.removeCallbacks(deferredPanRetune);
        pendingReliableSettingsRepeats = 0;
        return sendControlNow(action, logExplicitly, true);
    }

    private void cancelPendingNetworkEdits() {
        mainHandler.removeCallbacks(deferredSettingsApply);
        mainHandler.removeCallbacks(reliableSettingsRepeat);
        mainHandler.removeCallbacks(deferredPanRetune);
        mainHandler.removeCallbacks(serverStateRecovery);
        pendingReliableSettingsRepeats = 0;
        localSettingsGuardUntilMs = 0;
    }

    private boolean sendControlNow(String action, boolean logExplicitly, boolean guardLocalSettings) {
        boolean carriesSettings = NetworkControlProtocol.carriesSettings(action);
        if (carriesSettings) collectSettingsFromUi();
        if (carriesSettings && !isIndependentVfo() && serverProcessingMode != RadioSettings.PROCESSING_SERVER_SIDE) {
            try {
                JSONObject mode = new JSONObject();
                mode.put("processingMode", RadioSettings.PROCESSING_SERVER_SIDE);
                if (!client.sendControl("setProcessingMode", settings,
                        suppressServerCheck.isChecked(), fullResolutionSpectrumFrames, mode)) return false;
            } catch (JSONException e) {
                appendLog("Processing mode command failed: " + e.getMessage());
                return false;
            }
        }
        if (guardLocalSettings && ("settings".equals(action) || "start".equals(action))) {
            localSettingsGuardUntilMs = SystemClock.uptimeMillis() + REMOTE_ECHO_GUARD_MS;
        }
        boolean sent = client.sendControl(
                action,
                settings,
                suppressServerCheck.isChecked(),
                fullResolutionSpectrumFrames,
                null);
        if (sent && logExplicitly) {
            appendLog("Sent: " + action);
        }
        if (sent && carriesSettings && !isIndependentVfo()) {
            mainHandler.removeCallbacks(serverStateRecovery);
            mainHandler.postDelayed(serverStateRecovery, REMOTE_ECHO_GUARD_MS + 1000);
        }
        return sent;
    }

    private void sendReliableSettingsNow() {
        if (suppressUiCallbacks) {
            return;
        }
        if (isUsbOtgPreviewRunning()) {
            localSettingsGuardUntilMs = SystemClock.uptimeMillis() + REMOTE_ECHO_GUARD_MS;
            mainHandler.removeCallbacks(deferredUsbPreviewApply);
            applyUsbOtgPreviewSettings(false);
            return;
        }
        if (client == null || !client.isConnected() || !client.canControl()) {
            return;
        }
        mainHandler.removeCallbacks(deferredSettingsApply);
        mainHandler.removeCallbacks(reliableSettingsRepeat);
        pendingReliableSettingsRepeats = RELIABLE_SETTINGS_REPEAT_COUNT;
        sendControlNow("settings", false, true);
        mainHandler.postDelayed(reliableSettingsRepeat, RELIABLE_SETTINGS_REPEAT_MS);
    }

    private void showPriorityRequest(JSONObject command) {
        if (!client.canControl()) {
            return;
        }
        String requesterId = command.optString("requesterId");
        String requesterLabel = command.optString("requesterLabel", "unknown client");
        CheckBox blockCheck = new CheckBox(this);
        blockCheck.setText("Block further requests from this client");
        blockCheck.setPadding(dp(8), dp(4), dp(8), dp(4));
        new AlertDialog.Builder(this)
                .setTitle("Control request")
                .setMessage("Client " + requesterLabel + " requests receiver control.")
                .setView(blockCheck)
                .setPositiveButton("Allow", (dialog, which) ->
                        sendPriorityResponse(requesterId, true, blockCheck.isChecked()))
                .setNegativeButton("Deny", (dialog, which) ->
                        sendPriorityResponse(requesterId, false, blockCheck.isChecked()))
                .show();
    }

    private void sendPriorityResponse(String requesterId, boolean accepted, boolean blocked) {
        collectSettingsFromUi();
        try {
            JSONObject response = new JSONObject();
            response.put("requesterId", requesterId);
            response.put("accepted", accepted);
            response.put("blocked", blocked);
            client.sendControl("priorityResponse",
                    settings,
                    suppressServerCheck.isChecked(),
                    fullResolutionSpectrumFrames,
                    response);
        } catch (JSONException e) {
            appendLog("Priority response failed: " + e.getMessage());
        }
    }

    private void collectSettingsFromUi() {
        if (isIndependentVfo()) {
            settings.listeningFrequency = safeDouble(listenEdit.getText().toString(),
                    settings.listeningFrequency / 1e6) * 1e6;
            settings.centerFrequency = settings.listeningFrequency;
            settings.bandwidth = safeDouble(bandwidthEdit.getText().toString(), settings.bandwidth / 1000) * 1000;
            int[] modes = activeModulationIds();
            int mode = modulationSpinner.getSelectedItemPosition();
            if (mode >= 0 && mode < modes.length) settings.modulationType = modes[mode];
            int[] fft = activeFftLengths();
            int index = fftSpinner.getSelectedItemPosition();
            if (index >= 0 && index < fft.length) settings.fftLength = fft[index];
            settings.audioEnabled = audioCheck.isChecked();
            audioPlaybackEnabled = settings.audioEnabled;
            return;
        }
        if (isRtlUsbTarget()) {
            settings.inputMode = RadioSettings.INPUT_RF;
        }
        int sampleIndex = sampleRateSpinner.getSelectedItemPosition();
        double[] rates = activeSampleRates();
        if (sampleIndex >= 0 && sampleIndex < rates.length) {
            settings.sampleRate = rates[sampleIndex];
        }
        int fftIndex = fftSpinner.getSelectedItemPosition();
        int[] fftOptions = activeFftLengths();
        if (fftIndex >= 0 && fftIndex < fftOptions.length) {
            settings.fftLength = fftOptions[fftIndex];
        }
        settings.inputMode = isRtlUsbTarget()
                ? RadioSettings.INPUT_RF
                : Math.max(0, inputSpinner.getSelectedItemPosition());
        double requestedListening =
                safeDouble(listenEdit.getText().toString(), settings.listeningFrequency / 1_000_000.0) *
                        1_000_000.0;
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            settings.centerFrequency = 0.0;
            settings.actualFrequency = 0.0;
            settings.listeningFrequency = RadioSettings.clampDirectListeningFrequency(
                    settings.inputMode,
                    settings.sampleRate,
                    requestedListening);
        } else {
            settings.centerFrequency =
                    safeDouble(centerEdit.getText().toString(), settings.centerFrequency / 1_000_000.0) *
                            1_000_000.0;
            settings.centerFrequency = Math.max(minimumRfCenterHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.centerFrequency));
            settings.actualFrequency = settings.centerFrequency;
            settings.listeningFrequency = Math.max(minimumRfListeningHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, requestedListening));
        }
        settings.bandwidth = safeDouble(bandwidthEdit.getText().toString(), settings.bandwidth / 1_000.0) * 1_000.0;
        int modulationIndex = modulationSpinner.getSelectedItemPosition();
        int[] modeOptions = activeModulationIds();
        if (modulationIndex >= 0 && modulationIndex < modeOptions.length) {
            settings.modulationType = modeOptions[modulationIndex];
        }
        enforceListeningCenterBinding();
        settings.audioEnabled = audioCheck.isChecked();
        audioPlaybackEnabled = settings.audioEnabled;
    }

    private void enforceListeningCenterBinding() {
        if (!bindListeningToCenter || RadioSettings.isDirectInput(settings.inputMode)) {
            return;
        }
        settings.centerFrequency = Math.max(minimumRfCenterHz(),
                Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.centerFrequency));
        settings.actualFrequency = settings.centerFrequency;
        settings.listeningFrequency = settings.centerFrequency;
    }

    private void updateButtons() {
        boolean connected = client != null && client.isConnected();
        boolean active = client != null && client.isActive();
        boolean canControl = connected && client.canControl();
        boolean otgMode = isOtgMode();
        boolean otgPreviewRunning = isUsbOtgPreviewRunning();
        receiverModeButton.setText(otgMode ? "OTG" : "Net");
        connectButton.setText(otgMode ? "USB" : (active ? "Disconnect" : "Connect"));
        connectButton.setEnabled(otgMode || client != null);
        startButton.setEnabled(otgMode ? (usbSandbox != null && !otgPreviewRunning) : canControl);
        stopButton.setEnabled(otgMode ? otgPreviewRunning : canControl);
        settingsButton.setEnabled(otgMode ? usbSandbox != null : canControl);
        requestControlButton.setEnabled(!otgMode && connected && !client.canControl());
        independentVfoCheck.setEnabled(!active && !otgMode);
        remoteVfoSpinner.setEnabled(isIndependentVfo());
        reconnectCheck.setEnabled(!otgMode);
        boolean vfo = isIndependentVfo();
        requestControlButton.setVisibility(vfo ? View.GONE : View.VISIBLE);
        remoteVfoSpinner.setVisibility(vfo ? View.VISIBLE : View.GONE);
        vfoRangeLabel.setVisibility(vfo ? View.VISIBLE : View.GONE);
        suppressServerCheck.setEnabled(!vfo);
        fullResolutionSpectrumCheck.setEnabled(!vfo);
        bindTuningToCenterCheck.setEnabled(!vfo);
        sampleRateSpinner.setEnabled(!vfo);
        inputSpinner.setEnabled(!vfo && !isRtlUsbTarget());
        centerEdit.setEnabled(!vfo && !RadioSettings.isDirectInput(settings.inputMode));
        if (vfo) receiverModeButton.setText("VFO");
    }

    private void loadPrefs() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        client.setAutoReconnect(prefs.getBoolean("autoReconnect", true));
        client.setRemoteVfoMode(prefs.getBoolean("independentVfo", false));
        client.selectVfo(prefs.getInt("remoteVfoSlot", 0) == 0 ? "A" : "B");
        hostEdit.setText(prefs.getString("host", "127.0.0.1"));
        portEdit.setText(String.valueOf(prefs.getInt("port", DEFAULT_PORT)));
        settings.centerFrequency = Double.longBitsToDouble(prefs.getLong("center", Double.doubleToLongBits(settings.centerFrequency)));
        settings.listeningFrequency = Double.longBitsToDouble(prefs.getLong("listen", Double.doubleToLongBits(settings.listeningFrequency)));
        settings.sampleRate = Double.longBitsToDouble(prefs.getLong("sampleRate", Double.doubleToLongBits(settings.sampleRate)));
        settings.bandwidth = Double.longBitsToDouble(prefs.getLong("bandwidth", Double.doubleToLongBits(settings.bandwidth)));
        settings.inputMode = prefs.getInt("inputMode", settings.inputMode);
        settings.modulationType = prefs.getInt("modulation", settings.modulationType);
        settings.hfNoiseCancelDepth = Double.longBitsToDouble(
                prefs.getLong("hfNoiseCancelDepth", Double.doubleToLongBits(settings.hfNoiseCancelDepth)));
        settings.hfNoiseCancelRefGainDb = Double.longBitsToDouble(
                prefs.getLong("hfNoiseCancelRefGainDb", Double.doubleToLongBits(settings.hfNoiseCancelRefGainDb)));
        settings.hfNoiseCancelRefDelayNs = Double.longBitsToDouble(
                prefs.getLong("hfNoiseCancelRefDelayNs", Double.doubleToLongBits(settings.hfNoiseCancelRefDelayNs)));
        settings.hfNoiseCancelRefTiltDb = Double.longBitsToDouble(
                prefs.getLong("hfNoiseCancelRefTiltDb", Double.doubleToLongBits(settings.hfNoiseCancelRefTiltDb)));
        settings.hfNoiseCancelFreeze = prefs.getBoolean("hfNoiseCancelFreeze", settings.hfNoiseCancelFreeze);
        settings.audioEnabled = prefs.getBoolean("audio", settings.audioEnabled);
        settings.fftLength = prefs.getInt("fftLength", settings.fftLength);
        fullResolutionSpectrumFrames = prefs.getBoolean("fullResolutionSpectrumFrames", fullResolutionSpectrumFrames);
        fineTuneHoldMode = prefs.getBoolean("fineTuneHoldMode", fineTuneHoldMode);
        showGeneralBandMarkers = prefs.getBoolean("showGeneralBandMarkers", showGeneralBandMarkers);
        showAmateurBandMarkers = prefs.getBoolean("showAmateurBandMarkers", showAmateurBandMarkers);
        bindListeningToCenter = prefs.getBoolean("bindListeningToCenter", bindListeningToCenter);
        usbReceiverTarget = prefs.getInt("usbReceiverTarget", UsbSandbox.USB_TARGET_AUTO);
        if (usbReceiverTarget != UsbSandbox.USB_TARGET_RTL_SDR &&
                usbReceiverTarget != UsbSandbox.USB_TARGET_FOBOS) {
            usbReceiverTarget = UsbSandbox.USB_TARGET_AUTO;
        }
        receiverMode = prefs.getInt("receiverMode", receiverMode) == RECEIVER_MODE_OTG
                ? RECEIVER_MODE_OTG
                : RECEIVER_MODE_NETWORK;
        boolean suppressServer = prefs.getBoolean("suppressServer", true);
        displayLevelMin = prefs.getFloat("levelMin", displayLevelMin);
        displayLevelMax = prefs.getFloat("levelMax", displayLevelMax);
        if (displayLevelMax <= displayLevelMin + LEVEL_MIN_GAP_DB) {
            displayLevelMin = -130.0f;
            displayLevelMax = -50.0f;
        }
        spectrumZoomProgress = Math.max(0, Math.min(100,
                prefs.getInt("spectrumZoomProgress", spectrumZoomProgress)));
        normalizeSettingsForCurrentInput();

        updateControlsFromSettings(true);
        updateLevelControls();
        applyStoredZoomToSpectrum();
        suppressUiCallbacks = true;
        reconnectCheck.setChecked(prefs.getBoolean("autoReconnect", true));
        independentVfoCheck.setChecked(client.isRemoteVfoMode());
        replaceSpinnerOptions(modulationSpinner, modulationLabels());
        replaceSpinnerOptions(fftSpinner, fftLengthLabels());
        remoteVfoSpinner.setSelection(prefs.getInt("remoteVfoSlot", 0));
        audioCheck.setChecked(settings.audioEnabled);
        audioPlaybackEnabled = settings.audioEnabled;
        bindTuningToCenterCheck.setChecked(bindListeningToCenter);
        fullResolutionSpectrumCheck.setChecked(fullResolutionSpectrumFrames);
        generalBandMarkersCheck.setChecked(showGeneralBandMarkers);
        amateurBandMarkersCheck.setChecked(showAmateurBandMarkers);
        suppressServerCheck.setChecked(suppressServer);
        if (fineTuneDial != null) {
            fineTuneDial.setHoldOffsetMode(fineTuneHoldMode);
        }
        applyUsbReceiverTarget();
        updateFineTuneModeButton();
        updateBandMarkerVisibility();
        suppressUiCallbacks = false;
        updateControlsFromSettings(true);
    }

    private void savePrefs() {
        if (hostEdit == null) {
            return;
        }
        collectSettingsFromUi();
        getSharedPreferences(PREFS, MODE_PRIVATE)
                .edit()
                .putBoolean("autoReconnect", reconnectCheck.isChecked())
                .putBoolean("independentVfo", client.isRemoteVfoMode())
                .putInt("remoteVfoSlot", remoteVfoSpinner.getSelectedItemPosition())
                .putString("host", hostEdit.getText().toString().trim())
                .putInt("port", safeInt(portEdit.getText().toString(), DEFAULT_PORT))
                .putLong("center", Double.doubleToLongBits(settings.centerFrequency))
                .putLong("listen", Double.doubleToLongBits(settings.listeningFrequency))
                .putLong("sampleRate", Double.doubleToLongBits(settings.sampleRate))
                .putLong("bandwidth", Double.doubleToLongBits(settings.bandwidth))
                .putInt("fftLength", settings.fftLength)
                .putInt("inputMode", settings.inputMode)
                .putInt("modulation", settings.modulationType)
                .putLong("hfNoiseCancelDepth", Double.doubleToLongBits(settings.hfNoiseCancelDepth))
                .putLong("hfNoiseCancelRefGainDb", Double.doubleToLongBits(settings.hfNoiseCancelRefGainDb))
                .putLong("hfNoiseCancelRefDelayNs", Double.doubleToLongBits(settings.hfNoiseCancelRefDelayNs))
                .putLong("hfNoiseCancelRefTiltDb", Double.doubleToLongBits(settings.hfNoiseCancelRefTiltDb))
                .putBoolean("hfNoiseCancelFreeze", settings.hfNoiseCancelFreeze)
                .putBoolean("audio", settings.audioEnabled)
                .putBoolean("fullResolutionSpectrumFrames", fullResolutionSpectrumFrames)
                .putBoolean("fineTuneHoldMode", fineTuneHoldMode)
                .putBoolean("showGeneralBandMarkers", showGeneralBandMarkers)
                .putBoolean("showAmateurBandMarkers", showAmateurBandMarkers)
                .putBoolean("bindListeningToCenter", bindListeningToCenter)
                .putInt("usbReceiverTarget", usbReceiverTarget)
                .putInt("receiverMode", receiverMode)
                .putBoolean("suppressServer", suppressServerCheck.isChecked())
                .putFloat("levelMin", displayLevelMin)
                .putFloat("levelMax", displayLevelMax)
                .putInt("spectrumZoomProgress", spectrumZoomProgress)
                .apply();
    }

    private void updateControlsFromSettings(boolean force) {
        if (isOtgMode()) {
            normalizeSettingsForCurrentInput();
            normalizeSettingsForReceiverTarget();
        }
        refreshSampleRateOptions(false);
        if (isOtgMode()) enforceListeningCenterBinding();
        suppressUiCallbacks = true;
        setTextIfNotFocused(centerEdit, formatMhz(settings.centerFrequency), force);
        setTextIfNotFocused(listenEdit, formatMhz(settings.listeningFrequency), force);
        setTextIfNotFocused(bandwidthEdit, formatKhz(settings.bandwidth), force);
        sampleRateSpinner.setSelection(indexOfSampleRate(settings.sampleRate));
        fftSpinner.setSelection(indexOfFftLength(settings.fftLength));
        inputSpinner.setSelection(Math.max(0, Math.min(inputSpinner.getCount() - 1, settings.inputMode)));
        modulationSpinner.setSelection(indexOfModulation(settings.modulationType));
        updateReceiverTargetControls();
        boolean directInput = RadioSettings.isDirectInput(settings.inputMode);
        centerEdit.setEnabled(!directInput);
        centerEdit.setAlpha(directInput ? 0.55f : 1.0f);
        suppressUiCallbacks = false;
        updateFineTuneControl();
        if (isIndependentVfo()) updateButtons();
    }

    private void normalizeSettingsForCurrentInput() {
        if (!isFinite(settings.sampleRate) || settings.sampleRate <= 0.0) {
            settings.sampleRate = 8_000_000.0;
        }
        if (!isFinite(settings.bandwidth) || settings.bandwidth <= 0.0) {
            settings.bandwidth = RadioSettings.defaultBandwidthForModulation(settings.modulationType);
        }
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            settings.centerFrequency = 0.0;
            settings.actualFrequency = 0.0;
            settings.listeningFrequency = RadioSettings.clampDirectListeningFrequency(
                    settings.inputMode,
                    settings.sampleRate,
                    settings.listeningFrequency);
        } else {
            if (!isFinite(settings.centerFrequency) || settings.centerFrequency < RF_MIN_CENTER_FREQUENCY) {
                settings.centerFrequency = 100_000_000.0;
            }
            settings.centerFrequency = Math.max(minimumRfCenterHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.centerFrequency));
            settings.actualFrequency = settings.centerFrequency;
            if (!isFinite(settings.listeningFrequency) ||
                    settings.listeningFrequency < RF_MIN_LISTENING_FREQUENCY ||
                    settings.listeningFrequency > RF_EXPERIMENTAL_MAX_FREQUENCY) {
                settings.listeningFrequency = settings.centerFrequency;
            }
        }
    }

    private void setTextIfNotFocused(EditText editText, String text, boolean force) {
        if (editText == null || (!force && editText.hasFocus())) {
            return;
        }
        if (!text.contentEquals(editText.getText())) {
            editText.setText(text);
        }
    }

    private void setFrequencyTextsFromSettings(boolean force) {
        boolean previousSuppress = suppressUiCallbacks;
        suppressUiCallbacks = true;
        try {
            setTextIfNotFocused(centerEdit, formatMhz(settings.centerFrequency), force);
            setTextIfNotFocused(listenEdit, formatMhz(settings.listeningFrequency), force);
        } finally {
            suppressUiCallbacks = previousSuppress;
        }
    }

    private void scheduleSettingsApply() {
        scheduleSettingsApply(AUTO_APPLY_DELAY_MS);
    }

    private void scheduleSettingsApply(int delayMs) {
        if (suppressUiCallbacks) {
            return;
        }
        if (isUsbOtgPreviewRunning()) {
            localSettingsGuardUntilMs = SystemClock.uptimeMillis() + REMOTE_ECHO_GUARD_MS;
            collectSettingsFromUi();
            mainHandler.removeCallbacks(deferredUsbPreviewApply);
            mainHandler.postDelayed(deferredUsbPreviewApply, Math.max(0, delayMs));
            return;
        }
        if (client == null || !client.isConnected() || !client.canControl()) {
            return;
        }
        collectSettingsFromUi();
        localSettingsGuardUntilMs = SystemClock.uptimeMillis() + REMOTE_ECHO_GUARD_MS;
        mainHandler.removeCallbacks(reliableSettingsRepeat);
        pendingReliableSettingsRepeats = 0;
        mainHandler.removeCallbacks(deferredSettingsApply);
        mainHandler.postDelayed(deferredSettingsApply, Math.max(0, delayMs));
    }

    private void setControlsVisible(boolean visible) {
        controlsVisible = visible;
        if (visible) {
            settingsVisible = false;
            if (settingsScroll != null) {
                settingsScroll.setVisibility(View.GONE);
            }
        }
        controlsScroll.setVisibility(visible ? View.VISIBLE : View.GONE);
        controlsButton.setText(visible ? "Hide" : "Controls");
        if (settingsToggleButton != null) {
            settingsToggleButton.setText(settingsVisible ? "Hide" : "Settings");
        }
    }

    private void setSettingsVisible(boolean visible) {
        settingsVisible = visible;
        if (visible) {
            controlsVisible = false;
            controlsScroll.setVisibility(View.GONE);
        }
        if (settingsScroll != null) {
            settingsScroll.setVisibility(visible ? View.VISIBLE : View.GONE);
        }
        controlsButton.setText(controlsVisible ? "Hide" : "Controls");
        settingsToggleButton.setText(visible ? "Hide" : "Settings");
    }

    private void applyResponsiveLayout() {
        if (rootLayout == null || commandPanel == null || contentPanel == null) {
            return;
        }
        boolean landscape = getResources().getConfiguration().orientation ==
                Configuration.ORIENTATION_LANDSCAPE;
        rootLayout.removeAllViews();
        rootLayout.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        commandPanel.setOrientation(landscape ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        commandPanel.setPadding(
                dp(landscape ? 3 : 4),
                dp(landscape ? 4 : 3),
                dp(landscape ? 3 : 4),
                dp(landscape ? 4 : 3));
        rebuildCommandPanel(landscape);

        ViewGroup.LayoutParams controlsParams = controlsScroll.getLayoutParams();
        if (controlsParams instanceof LinearLayout.LayoutParams) {
            ((LinearLayout.LayoutParams) controlsParams).height = dp(landscape ? 188 : 230);
            controlsScroll.setLayoutParams(controlsParams);
        }
        ViewGroup.LayoutParams settingsParams = settingsScroll.getLayoutParams();
        if (settingsParams instanceof LinearLayout.LayoutParams) {
            ((LinearLayout.LayoutParams) settingsParams).height = dp(landscape ? 188 : 230);
            settingsScroll.setLayoutParams(settingsParams);
        }
        ViewGroup.LayoutParams levelParams = levelPanel.getLayoutParams();
        if (levelParams instanceof LinearLayout.LayoutParams) {
            ((LinearLayout.LayoutParams) levelParams).height = dp(landscape ? 44 : 52);
            levelPanel.setLayoutParams(levelParams);
        }
        if (zoomBottomPanel != null) {
            zoomBottomPanel.setVisibility(landscape ? View.GONE : View.VISIBLE);
        }

        if (landscape) {
            rootLayout.addView(commandPanel, new LinearLayout.LayoutParams(
                    dp(92), LinearLayout.LayoutParams.MATCH_PARENT));
            rootLayout.addView(contentPanel, new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.MATCH_PARENT, 1.0f));
            rootLayout.addView(zoomSidePanel, new LinearLayout.LayoutParams(
                    dp(48), LinearLayout.LayoutParams.MATCH_PARENT));
        } else {
            rootLayout.addView(commandPanel, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(48)));
            rootLayout.addView(contentPanel, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));
        }
    }

    private void rebuildCommandPanel(boolean landscape) {
        commandPanel.removeAllViews();
        Button[] buttons = new Button[] {
                receiverModeButton, startButton, stopButton, settingsButton, controlsButton, settingsToggleButton
        };
        for (Button button : buttons) {
            button.setTextSize(landscape ? 11.0f : 12.0f);
            LinearLayout.LayoutParams params = landscape
                    ? new LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f)
                    : weightButtonParams();
            params.setMargins(dp(landscape ? 0 : 1), dp(landscape ? 1 : 0),
                    dp(landscape ? 0 : 1), dp(landscape ? 1 : 0));
            commandPanel.addView(button, params);
        }
    }

    private LinearLayout buildLevelPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.HORIZONTAL);
        panel.setGravity(Gravity.CENTER_VERTICAL);
        panel.setPadding(dp(6), dp(2), dp(6), dp(2));
        panel.setBackgroundColor(0xff101418);

        levelMinLabel = label("");
        levelMinLabel.setTextSize(11.0f);
        levelMaxLabel = label("");
        levelMaxLabel.setTextSize(11.0f);
        fineTuneLabel = label("");
        fineTuneLabel.setTextSize(10.0f);
        fineTuneLabel.setGravity(Gravity.CENTER);
        fineTuneModeButton = new View(this);
        fineTuneModeButton.setOnClickListener(v -> {
            fineTuneHoldMode = !fineTuneHoldMode;
            if (fineTuneDial != null) {
                fineTuneDial.setHoldOffsetMode(fineTuneHoldMode);
            }
            updateFineTuneModeButton();
            savePrefs();
        });

        levelMinSeek = levelSeekBar();
        levelMaxSeek = levelSeekBar();
        levelMinSeek.setOnSeekBarChangeListener(levelChangeListener(true));
        levelMaxSeek.setOnSeekBarChangeListener(levelChangeListener(false));
        fineTuneDial = new FineTuneDialView(this);
        fineTuneDial.setListener(deltaHz -> applyFineTuneDelta(deltaHz));
        fineTuneDial.setModeListener(enabled -> {
            fineTuneHoldMode = enabled;
            updateFineTuneModeButton();
            savePrefs();
        });

        panel.addView(levelMinLabel, new LinearLayout.LayoutParams(dp(58), dp(38)));
        panel.addView(levelMinSeek, new LinearLayout.LayoutParams(0, dp(38), 1.0f));
        panel.addView(levelMaxLabel, new LinearLayout.LayoutParams(dp(58), dp(38)));
        panel.addView(levelMaxSeek, new LinearLayout.LayoutParams(0, dp(38), 1.0f));
        panel.addView(fineTuneLabel, new LinearLayout.LayoutParams(dp(72), dp(42)));
        LinearLayout.LayoutParams modeParams = new LinearLayout.LayoutParams(dp(18), dp(18));
        modeParams.setMargins(0, 0, dp(4), 0);
        panel.addView(fineTuneModeButton, modeParams);
        panel.addView(fineTuneDial, new LinearLayout.LayoutParams(0, dp(46), 1.1f));
        updateFineTuneControl();
        updateFineTuneModeButton();
        return panel;
    }

    private LinearLayout buildBottomZoomPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.HORIZONTAL);
        panel.setGravity(Gravity.CENTER_VERTICAL);
        panel.setPadding(dp(6), dp(2), dp(6), dp(2));
        panel.setBackgroundColor(0xff101418);

        zoomBottomLabel = label("");
        zoomBottomLabel.setTextSize(11.0f);
        zoomBottomLabel.setGravity(Gravity.CENTER);
        zoomBottomSeek = zoomSeekBar();
        zoomBottomSeek.setOnSeekBarChangeListener(zoomChangeListener());

        panel.addView(zoomBottomLabel, new LinearLayout.LayoutParams(dp(62), dp(38)));
        panel.addView(zoomBottomSeek, new LinearLayout.LayoutParams(0, dp(38), 1.0f));
        updateZoomControls();
        return panel;
    }

    private LinearLayout buildSideZoomPanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER);
        panel.setPadding(dp(2), dp(6), dp(2), dp(6));
        panel.setBackgroundColor(0xff101418);

        zoomSideLabel = label("");
        zoomSideLabel.setTextSize(10.0f);
        zoomSideLabel.setGravity(Gravity.CENTER);
        zoomSideSeek = zoomSeekBar();
        zoomSideSeek.setRotation(-90.0f);
        zoomSideSeek.setOnSeekBarChangeListener(zoomChangeListener());

        FrameLayout sliderFrame = new FrameLayout(this);
        sliderFrame.addView(zoomSideSeek, new FrameLayout.LayoutParams(
                dp(190), dp(42), Gravity.CENTER));

        panel.addView(zoomSideLabel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(44)));
        panel.addView(sliderFrame, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));
        updateZoomControls();
        return panel;
    }

    private SeekBar zoomSeekBar() {
        SeekBar seekBar = new SeekBar(this);
        seekBar.setMax(100);
        seekBar.setProgress(spectrumZoomProgress);
        return seekBar;
    }

    private SeekBar.OnSeekBarChangeListener zoomChangeListener() {
        return new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (suppressZoomCallbacks || !fromUser) {
                    return;
                }
                spectrumZoomProgress = Math.max(0, Math.min(100, progress));
                if (spectrumView != null) {
                    spectrumView.setZoomProgress(spectrumZoomProgress);
                }
                updateZoomControls();
                updateFineTuneControl();
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                savePrefs();
            }
        };
    }

    private SeekBar levelSeekBar() {
        SeekBar seekBar = new SeekBar(this);
        seekBar.setMax(LEVEL_DB_MAX - LEVEL_DB_MIN);
        return seekBar;
    }

    private SeekBar.OnSeekBarChangeListener levelChangeListener(boolean minSlider) {
        return new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (suppressLevelCallbacks || !fromUser) {
                    return;
                }
                handleLevelChange(minSlider);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                savePrefs();
            }
        };
    }

    private void handleLevelChange(boolean minSlider) {
        int minDb = progressToLevelDb(levelMinSeek.getProgress());
        int maxDb = progressToLevelDb(levelMaxSeek.getProgress());
        if (minDb > maxDb - LEVEL_MIN_GAP_DB) {
            if (minSlider) {
                minDb = maxDb - LEVEL_MIN_GAP_DB;
            } else {
                maxDb = minDb + LEVEL_MIN_GAP_DB;
            }
        }
        minDb = Math.max(LEVEL_DB_MIN, Math.min(LEVEL_DB_MAX - LEVEL_MIN_GAP_DB, minDb));
        maxDb = Math.max(LEVEL_DB_MIN + LEVEL_MIN_GAP_DB, Math.min(LEVEL_DB_MAX, maxDb));
        displayLevelMin = minDb;
        displayLevelMax = maxDb;
        updateLevelControls();
    }

    private void updateLevelControls() {
        if (levelMinSeek == null || levelMaxSeek == null) {
            return;
        }
        suppressLevelCallbacks = true;
        int minDb = Math.round(displayLevelMin);
        int maxDb = Math.round(displayLevelMax);
        minDb = Math.max(LEVEL_DB_MIN, Math.min(LEVEL_DB_MAX - LEVEL_MIN_GAP_DB, minDb));
        maxDb = Math.max(minDb + LEVEL_MIN_GAP_DB, Math.min(LEVEL_DB_MAX, maxDb));
        displayLevelMin = minDb;
        displayLevelMax = maxDb;
        levelMinSeek.setProgress(levelDbToProgress(minDb));
        levelMaxSeek.setProgress(levelDbToProgress(maxDb));
        levelMinLabel.setText(String.format(Locale.US, "Min\n%d", minDb));
        levelMaxLabel.setText(String.format(Locale.US, "Max\n%d", maxDb));
        if (spectrumView != null) {
            spectrumView.setLevelRange(displayLevelMin, displayLevelMax);
        }
        suppressLevelCallbacks = false;
    }

    private void applyStoredZoomToSpectrum() {
        if (spectrumView == null) {
            return;
        }
        spectrumView.setZoomProgress(spectrumZoomProgress);
        updateZoomControls();
    }

    private void updateZoomControls() {
        suppressZoomCallbacks = true;
        int progress = Math.max(0, Math.min(100, spectrumZoomProgress));
        spectrumZoomProgress = progress;
        if (zoomBottomSeek != null) {
            zoomBottomSeek.setProgress(progress);
        }
        if (zoomSideSeek != null) {
            zoomSideSeek.setProgress(progress);
        }
        String label = String.format(Locale.US, "Zoom\nx%.1f",
                Math.pow(100.0, progress / 100.0));
        if (zoomBottomLabel != null) {
            zoomBottomLabel.setText(label);
        }
        if (zoomSideLabel != null) {
            zoomSideLabel.setText(label);
        }
        suppressZoomCallbacks = false;
    }

    private void updateFineTuneControl() {
        if (fineTuneDial == null || fineTuneLabel == null) {
            return;
        }
        double spanHz = spectrumView != null ? spectrumView.visibleSpanHz() : settings.sampleRate;
        fineTuneDial.setVisibleSpanHz(spanHz);
        fineTuneDial.setHoldOffsetMode(fineTuneHoldMode);
        fineTuneLabel.setText(String.format(Locale.US,
                "Fine +/- %s",
                formatFineTuneRange(fineTuneDial.getRangeHz())));
        updateFineTuneModeButton();
    }

    private void updateBandMarkerVisibility() {
        if (spectrumView != null) {
            spectrumView.setBandMarkersEnabled(showGeneralBandMarkers, showAmateurBandMarkers);
        }
    }

    private void updateFineTuneModeButton() {
        if (fineTuneModeButton == null) {
            return;
        }
        fineTuneModeButton.setBackgroundColor(fineTuneHoldMode ? 0xffd65050 : 0xff3dbb68);
        fineTuneModeButton.setContentDescription(fineTuneHoldMode
                ? "Held fine tuning offset"
                : "Temporary fine tuning");
    }

    private void applyFineTuneDelta(double deltaHz) {
        if (Double.isNaN(deltaHz) || Double.isInfinite(deltaHz) || Math.abs(deltaHz) < 0.01) {
            return;
        }
        collectSettingsFromUi();
        settings.listeningFrequency += deltaHz;
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            settings.centerFrequency = 0.0;
            settings.actualFrequency = 0.0;
            settings.listeningFrequency = RadioSettings.clampDirectListeningFrequency(
                    settings.inputMode,
                    settings.sampleRate,
                    settings.listeningFrequency);
        } else {
            settings.listeningFrequency = Math.max(minimumRfListeningHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.listeningFrequency));
            if (bindListeningToCenter) {
                settings.centerFrequency = Math.max(minimumRfCenterHz(),
                        Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.listeningFrequency));
                settings.actualFrequency = settings.centerFrequency;
                settings.listeningFrequency = settings.centerFrequency;
            } else {
                double halfRate = Math.max(1.0, settings.sampleRate * 0.5);
                if (settings.listeningFrequency < settings.centerFrequency - halfRate) {
                    settings.centerFrequency = Math.max(minimumRfCenterHz(),
                            Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.listeningFrequency + halfRate));
                    settings.actualFrequency = settings.centerFrequency;
                } else if (settings.listeningFrequency > settings.centerFrequency + halfRate) {
                    settings.centerFrequency = Math.max(minimumRfCenterHz(),
                            Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.listeningFrequency - halfRate));
                    settings.actualFrequency = settings.centerFrequency;
                }
            }
        }
        setFrequencyTextsFromSettings(true);
        scheduleSettingsApply(FAST_APPLY_DELAY_MS);
    }

    private void tuneToFrequency(double frequencyHz, boolean immediate) {
        if (!isFinite(frequencyHz)) {
            return;
        }
        if (isIndependentVfo()) {
            collectSettingsFromUi();
            settings.listeningFrequency = frequencyHz;
            settings.centerFrequency = frequencyHz;
            setFrequencyTextsFromSettings(true);
            if (immediate) sendReliableSettingsNow(); else scheduleSettingsApply(FAST_APPLY_DELAY_MS);
            return;
        }
        double beforeCenter = settings.centerFrequency;
        double beforeListen = settings.listeningFrequency;
        collectSettingsFromUi();
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            settings.centerFrequency = 0.0;
            settings.actualFrequency = 0.0;
            settings.listeningFrequency = RadioSettings.clampDirectListeningFrequency(
                    settings.inputMode,
                    settings.sampleRate,
                    frequencyHz);
        } else {
            double clamped = Math.max(minimumRfListeningHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, frequencyHz));
            if (bindListeningToCenter) {
                settings.centerFrequency = Math.max(minimumRfCenterHz(),
                        Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, clamped));
                settings.actualFrequency = settings.centerFrequency;
                settings.listeningFrequency = settings.centerFrequency;
            } else {
                settings.listeningFrequency = clamped;
                keepRfListeningInsideVisiblePassband();
            }
        }
        setFrequencyTextsFromSettings(true);
        if (spectrumView != null) {
            spectrumView.setListeningFrequency(settings.listeningFrequency);
        }
        Log.d(USB_LOG_TAG, String.format(Locale.US,
                "tuneToFrequency tap %.3f MHz center %.3f->%.3f listen %.3f->%.3f input %d",
                frequencyHz / 1_000_000.0,
                beforeCenter / 1_000_000.0,
                settings.centerFrequency / 1_000_000.0,
                beforeListen / 1_000_000.0,
                settings.listeningFrequency / 1_000_000.0,
                settings.inputMode));
        if (immediate) {
            sendReliableSettingsNow();
        } else {
            scheduleSettingsApply(FAST_APPLY_DELAY_MS);
        }
    }

    private void centerToFrequency(double frequencyHz, boolean immediate) {
        if (isIndependentVfo()) { tuneToFrequency(frequencyHz, immediate); return; }
        if (!isFinite(frequencyHz)) {
            return;
        }
        double beforeCenter = settings.centerFrequency;
        double beforeListen = settings.listeningFrequency;
        collectSettingsFromUi();
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            return;
        }
        double oldCenter = settings.centerFrequency;
        settings.centerFrequency = Math.max(minimumRfCenterHz(),
                Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, frequencyHz));
        settings.actualFrequency = settings.centerFrequency;
        double appliedDelta = settings.centerFrequency - oldCenter;
        if (bindListeningToCenter) {
            settings.listeningFrequency = settings.centerFrequency;
        }
        setFrequencyTextsFromSettings(true);
        if (spectrumView != null) {
            if (Math.abs(appliedDelta) >= 1.0) {
                spectrumView.shiftFrequencyDisplay(appliedDelta);
            }
            spectrumView.setListeningFrequency(settings.listeningFrequency);
        }
        Log.d(USB_LOG_TAG, String.format(Locale.US,
                "centerToFrequency doubleTap %.3f MHz center %.3f->%.3f listen %.3f->%.3f bind %s",
                frequencyHz / 1_000_000.0,
                beforeCenter / 1_000_000.0,
                settings.centerFrequency / 1_000_000.0,
                beforeListen / 1_000_000.0,
                settings.listeningFrequency / 1_000_000.0,
                bindListeningToCenter));
        if (immediate) {
            sendReliableSettingsNow();
        } else {
            scheduleSettingsApply(FAST_APPLY_DELAY_MS);
        }
    }

    private void applySpectrumPanDelta(double deltaHz) {
        if (isIndependentVfo()) {
            tuneToFrequency(settings.listeningFrequency + deltaHz, true);
            return;
        }
        if (!isFinite(deltaHz) || Math.abs(deltaHz) < 1.0) {
            return;
        }
        collectSettingsFromUi();
        double beforeCenter = settings.centerFrequency;
        double beforeListen = settings.listeningFrequency;
        double appliedDelta;
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            // Direct inputs have no tunable RF center in this preview path; keep the audio marker stable.
            settings.centerFrequency = 0.0;
            settings.actualFrequency = 0.0;
            appliedDelta = 0.0;
        } else {
            double oldCenter = settings.centerFrequency;
            settings.centerFrequency = Math.max(minimumRfCenterHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.centerFrequency + deltaHz));
            appliedDelta = settings.centerFrequency - oldCenter;
            settings.actualFrequency = settings.centerFrequency;
            if (bindListeningToCenter) {
                settings.listeningFrequency = settings.centerFrequency;
            }
        }
        if (Math.abs(appliedDelta) < 1.0) {
            return;
        }
        if (spectrumView != null) {
            spectrumView.shiftFrequencyDisplay(appliedDelta);
            spectrumView.setListeningFrequency(settings.listeningFrequency);
        }
        setFrequencyTextsFromSettings(true);
        Log.d(USB_LOG_TAG, String.format(Locale.US,
                "pan delta %.0f applied %.0f center %.3f->%.3f listen %.3f->%.3f input %d",
                deltaHz,
                appliedDelta,
                beforeCenter / 1_000_000.0,
                settings.centerFrequency / 1_000_000.0,
                beforeListen / 1_000_000.0,
                settings.listeningFrequency / 1_000_000.0,
                settings.inputMode));
        requestLivePanRetune();
    }

    private void requestLivePanRetune() {
        long now = SystemClock.uptimeMillis();
        localSettingsGuardUntilMs = now + REMOTE_ECHO_GUARD_MS;
        long elapsed = now - lastPanRetuneMs;
        if (elapsed >= PAN_RETUNE_INTERVAL_MS) {
            mainHandler.removeCallbacks(deferredPanRetune);
            lastPanRetuneMs = now;
            applyRetuneSettingsNow(false);
            return;
        }
        mainHandler.removeCallbacks(deferredPanRetune);
        mainHandler.postDelayed(deferredPanRetune,
                Math.max(PAN_APPLY_DELAY_MS, PAN_RETUNE_INTERVAL_MS - elapsed));
    }

    private void applyRetuneSettingsNow(boolean logExplicitly) {
        if (isUsbOtgPreviewRunning()) {
            applyUsbOtgPreviewSettings(logExplicitly, false);
            return;
        }
        if (client != null && client.isConnected() && client.canControl()) {
            sendControlNow("settings", logExplicitly, true);
        }
    }

    private void keepRfListeningInsideVisiblePassband() {
        if (RadioSettings.isDirectInput(settings.inputMode)) {
            return;
        }
        double halfRate = Math.max(1.0, settings.sampleRate * 0.5);
        if (settings.listeningFrequency < settings.centerFrequency - halfRate) {
            settings.centerFrequency = Math.max(minimumRfCenterHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.listeningFrequency + halfRate));
            settings.actualFrequency = settings.centerFrequency;
        } else if (settings.listeningFrequency > settings.centerFrequency + halfRate) {
            settings.centerFrequency = Math.max(minimumRfCenterHz(),
                    Math.min(RF_EXPERIMENTAL_MAX_FREQUENCY, settings.listeningFrequency - halfRate));
            settings.actualFrequency = settings.centerFrequency;
        }
    }

    private String formatFineTuneRange(double hz) {
        if (hz >= 1_000_000.0) {
            return String.format(Locale.US, "%.1fM", hz / 1_000_000.0);
        }
        if (hz >= 1_000.0) {
            return String.format(Locale.US, "%.0fk", hz / 1_000.0);
        }
        return String.format(Locale.US, "%.0f", hz);
    }

    private int levelDbToProgress(int db) {
        return Math.max(0, Math.min(LEVEL_DB_MAX - LEVEL_DB_MIN, db - LEVEL_DB_MIN));
    }

    private int progressToLevelDb(int progress) {
        return LEVEL_DB_MIN + Math.max(0, Math.min(LEVEL_DB_MAX - LEVEL_DB_MIN, progress));
    }

    private void addAutoApplyTextWatcher(EditText editText) {
        editText.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                scheduleSettingsApply();
            }

            @Override
            public void afterTextChanged(Editable s) {
            }
        });
    }

    private AdapterView.OnItemSelectedListener autoApplySpinnerListener(boolean modePreset) {
        return new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (suppressUiCallbacks) {
                    return;
                }
                if (parent == sampleRateSpinner) {
                    double[] rates = activeSampleRates();
                    if (position >= 0 && position < rates.length &&
                            Math.abs(rates[position] - settings.sampleRate) < 0.5) return;
                } else if (parent == fftSpinner) {
                    int[] lengths = activeFftLengths();
                    if (position >= 0 && position < lengths.length &&
                            lengths[position] == settings.fftLength) return;
                } else if (parent == modulationSpinner) {
                    int[] modes = activeModulationIds();
                    if (position >= 0 && position < modes.length &&
                            modes[position] == settings.modulationType) return;
                }
                int[] modes = activeModulationIds();
                if (modePreset && position >= 0 && position < modes.length) {
                    settings.modulationType = modes[position];
                    bandwidthEdit.setText(formatKhz(RadioSettings.defaultBandwidthForModulation(settings.modulationType)));
                }
                sendReliableSettingsNow();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        };
    }

    private AdapterView.OnItemSelectedListener inputModeSpinnerListener() {
        return new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (suppressUiCallbacks) {
                    return;
                }
                int nextMode = Math.max(RadioSettings.INPUT_RF,
                        Math.min(RadioSettings.INPUT_HF_NOISE_CANCEL, position));
                if (settings.inputMode == nextMode) return;
                if (settings.inputMode != nextMode) {
                    settings.inputMode = nextMode;
                    int sampleIndex = sampleRateSpinner.getSelectedItemPosition();
                    double[] rates = activeSampleRates();
                    if (sampleIndex >= 0 && sampleIndex < rates.length) {
                        settings.sampleRate = rates[sampleIndex];
                    }
                    if (RadioSettings.isDirectInput(nextMode)) {
                        settings.centerFrequency = 0.0;
                        settings.actualFrequency = 0.0;
                        settings.listeningFrequency =
                                nextMode == RadioSettings.INPUT_HF_COMBINED ? 0.0 : 1_250_000.0;
                    } else {
                        settings.centerFrequency = 100_000_000.0;
                        settings.actualFrequency = 100_000_000.0;
                        settings.listeningFrequency = 100_000_000.0;
                    }
                    updateControlsFromSettings(true);
                }
                sendReliableSettingsNow();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        };
    }

    private LinearLayout row() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        layout.setPadding(dp(4), dp(3), dp(4), dp(3));
        return layout;
    }

    private EditText edit(String hint) {
        EditText view = new EditText(this);
        view.setSingleLine(true);
        view.setHint(hint);
        view.setTextColor(0xff101418);
        view.setHintTextColor(0xff60717d);
        view.setTextSize(14.0f);
        view.setBackgroundColor(0xffffffff);
        view.setPadding(dp(8), 0, dp(8), 0);
        view.setInputType(InputType.TYPE_CLASS_NUMBER |
                InputType.TYPE_NUMBER_FLAG_DECIMAL |
                InputType.TYPE_NUMBER_FLAG_SIGNED);
        return view;
    }

    private Button button(String text) {
        Button view = new Button(this);
        view.setText(text);
        view.setAllCaps(false);
        view.setTextSize(12.0f);
        return view;
    }

    private TextView label(String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(0xffdbe6ec);
        view.setTextSize(14.0f);
        return view;
    }

    private Spinner spinner(String[] values) {
        Spinner view = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(
                this,
                android.R.layout.simple_spinner_item,
                values);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        view.setAdapter(adapter);
        view.setBackgroundColor(0xffffffff);
        return view;
    }

    private View fieldWithLabel(String label, View field) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(3), 0, dp(3), 0);
        TextView text = label(label);
        text.setTextSize(11.0f);
        layout.addView(text, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(18)));
        layout.addView(field, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(40)));
        return layout;
    }

    private LinearLayout.LayoutParams weightButtonParams() {
        return new LinearLayout.LayoutParams(0, dp(42), 1.0f);
    }

    private double[] activeSampleRates() {
        if (isIndependentVfo()) return new double[]{settings.sampleRate};
        double[] rates = effectiveUsbReceiverTarget() == UsbSandbox.USB_TARGET_RTL_SDR ? rtlSampleRates : fobosSampleRates;
        if (isOtgMode() || !hasServerSettings || !isFinite(serverSettings.sampleRate) ||
                serverSettings.sampleRate <= 0.0) return rates;
        for (double rate : rates) if (Math.abs(rate - serverSettings.sampleRate) < 0.5) return rates;
        double[] result = java.util.Arrays.copyOf(rates, rates.length + 1);
        result[rates.length] = serverSettings.sampleRate;
        return result;
    }

    private int[] activeFftLengths() {
        if (isIndependentVfo()) return new int[]{512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
        return withServerOption(fftLengths, serverSettings.fftLength);
    }

    private int[] activeModulationIds() {
        if (isIndependentVfo()) return new int[]{0, 1, 2, 3, 4, 5, 6, 7};
        return withServerOption(modulationIds, serverSettings.modulationType);
    }

    private int[] withServerOption(int[] values, int serverValue) {
        if (isOtgMode() || !hasServerSettings || serverValue < 0) return values;
        for (int value : values) if (value == serverValue) return values;
        int[] result = java.util.Arrays.copyOf(values, values.length + 1);
        result[values.length] = serverValue;
        return result;
    }

    private void replaceSpinnerOptions(Spinner spinner, String[] labels) {
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, labels);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
    }

    private void refreshSampleRateOptions(boolean force) {
        if (sampleRateSpinner == null) {
            return;
        }
        boolean rtlOptions = effectiveUsbReceiverTarget() == UsbSandbox.USB_TARGET_RTL_SDR;
        if (!force && sampleRateOptionsAreRtl == rtlOptions &&
                sampleRateSpinner.getAdapter() != null) {
            return;
        }
        sampleRateOptionsAreRtl = rtlOptions;
        boolean previousSuppress = suppressUiCallbacks;
        suppressUiCallbacks = true;
        ArrayAdapter<String> adapter = new ArrayAdapter<>(
                this,
                android.R.layout.simple_spinner_item,
                sampleRateLabels());
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sampleRateSpinner.setAdapter(adapter);
        settings.sampleRate = nearestSampleRate(settings.sampleRate);
        sampleRateSpinner.setSelection(indexOfSampleRate(settings.sampleRate), false);
        suppressUiCallbacks = previousSuppress;
    }

    private double nearestSampleRate(double value) {
        double[] rates = activeSampleRates();
        if (rates.length == 0) {
            return value;
        }
        if (!isFinite(value) || value <= 0.0) {
            return effectiveUsbReceiverTarget() == UsbSandbox.USB_TARGET_RTL_SDR ? 2_400_000.0 : rates[0];
        }
        if (effectiveUsbReceiverTarget() == UsbSandbox.USB_TARGET_RTL_SDR && value > 3_200_000.0) {
            return 2_400_000.0;
        }
        return rates[indexOfSampleRate(value)];
    }

    private String[] sampleRateLabels() {
        double[] rates = activeSampleRates();
        String[] labels = new String[rates.length];
        for (int i = 0; i < rates.length; ++i) {
            labels[i] = formatMhz(rates[i]);
        }
        return labels;
    }

    private String[] fftLengthLabels() {
        int[] values = activeFftLengths();
        String[] labels = new String[values.length];
        for (int i = 0; i < values.length; ++i) {
            labels[i] = String.format(Locale.US, "%d", values[i]);
        }
        return labels;
    }

    private String[] modulationLabels() {
        int[] values = activeModulationIds();
        String[] labels = new String[values.length];
        for (int i = 0; i < values.length; ++i) {
            labels[i] = i < modulationIds.length ? RadioSettings.modulationLabel(values[i])
                    : "Server mode " + values[i];
        }
        return labels;
    }

    private int indexOfSampleRate(double value) {
        double[] rates = activeSampleRates();
        int best = 0;
        double bestDelta = Double.MAX_VALUE;
        for (int i = 0; i < rates.length; ++i) {
            double delta = Math.abs(rates[i] - value);
            if (delta < bestDelta) {
                bestDelta = delta;
                best = i;
            }
        }
        return best;
    }

    private int indexOfFftLength(int value) {
        int[] values = activeFftLengths();
        int best = 0;
        int bestDelta = Integer.MAX_VALUE;
        for (int i = 0; i < values.length; ++i) {
            int delta = Math.abs(values[i] - value);
            if (delta < bestDelta) {
                bestDelta = delta;
                best = i;
            }
        }
        return best;
    }

    private int indexOfModulation(int value) {
        int[] values = activeModulationIds();
        for (int i = 0; i < values.length; ++i) {
            if (values[i] == value) {
                return i;
            }
        }
        return 0;
    }

    private void appendLog(String text) {
        String previous = logText.getText().toString();
        String next = previous.isEmpty() ? text : previous + "\n" + text;
        int maxLength = 7000;
        if (next.length() > maxLength) {
            next = next.substring(next.length() - maxLength);
        }
        logText.setText(next);
    }

    private void appendUsbLog(String text) {
        String safeText = text != null ? text : "<null USB log>";
        Log.d(USB_LOG_TAG, safeText);
        runOnMain(() -> {
            if (statusText != null) {
                int newline = safeText.indexOf('\n');
                statusText.setText(newline >= 0 ? safeText.substring(0, newline) : safeText);
            }
            appendLog(safeText);
        });
    }

    private void appendUsbLogIfNotEmpty(String text) {
        if (text != null && !text.isEmpty()) {
            appendUsbLog(text);
        }
    }

    private void runOnMain(Runnable runnable) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            runnable.run();
        } else {
            mainHandler.post(runnable);
        }
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static int safeInt(String text, int fallback) {
        try {
            return Integer.parseInt(text.trim());
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    private static double safeDouble(String text, double fallback) {
        try {
            return Double.parseDouble(text.trim().replace(',', '.'));
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    private static boolean isFinite(double value) {
        return !Double.isNaN(value) && !Double.isInfinite(value);
    }

    private static String formatMhz(double hz) {
        return String.format(Locale.US, "%.6f", hz / 1_000_000.0);
    }

    private static String formatKhz(double hz) {
        double khz = hz / 1_000.0;
        if (Math.abs(khz - Math.rint(khz)) < 0.01) {
            return String.format(Locale.US, "%.0f", khz);
        }
        return String.format(Locale.US, "%.1f", khz);
    }
}
