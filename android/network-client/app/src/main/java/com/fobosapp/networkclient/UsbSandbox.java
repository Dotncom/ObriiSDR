package com.fobosapp.networkclient;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.hardware.usb.UsbRequest;
import android.os.Build;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

final class UsbSandbox {
    interface Listener {
        void onUsbLog(String message);
        void onUsbSpectrum(FobosNetworkClient.SpectrumFrame frame);
        void onUsbAudio(byte[] pcmData);
        void onUsbTelemetry(String message);
    }

    static final int FOBOS_VENDOR_ID = 0x16d0;
    static final int FOBOS_PRODUCT_ID = 0x132e;
    static final int FOBOS_STANDARD_BCD_DEVICE = 0x0000;
    static final int FOBOS_AGILE_BCD_DEVICE = 0x0101;
    static final int REALTEK_VENDOR_ID = 0x0bda;
    static final int RTL2832_PRODUCT_ID = 0x2832;
    static final int RTL2838_PRODUCT_ID = 0x2838;
    static final int TERRATEC_VENDOR_ID = 0x0ccd;
    static final int TERRATEC_RTL_E4000_PRODUCT_ID = 0x00a9;
    static final int TERRATEC_RTL_R820T_PRODUCT_ID = 0x00b3;
    static final int USB_TARGET_AUTO = 0;
    static final int USB_TARGET_FOBOS = 1;
    static final int USB_TARGET_RTL_SDR = 2;

    private static final String ACTION_USB_PERMISSION =
            "com.fobosapp.networkclient.USB_PERMISSION";
    private static final String LOG_TAG = "FobosUsbSandbox";
    private static final int USB_VENDOR_IN = UsbConstants.USB_TYPE_VENDOR | UsbConstants.USB_DIR_IN;
    private static final int USB_VENDOR_OUT = UsbConstants.USB_TYPE_VENDOR | UsbConstants.USB_DIR_OUT;
    private static final int RTL_USB_SYSCTL = 0x2000;
    private static final int RTL_USB_EPA_CTL = 0x2148;
    private static final int RTL_USB_EPA_MAXPKT = 0x2158;
    private static final int RTL_DEMOD_CTL = 0x3000;
    private static final int RTL_DEMOD_CTL_1 = 0x300b;
    private static final int RTL_BLOCK_DEMOD = 0;
    private static final int RTL_BLOCK_USB = 1;
    private static final int RTL_BLOCK_SYS = 2;
    private static final int RTL_BLOCK_I2C = 6;
    private static final int RTL_XTAL_HZ = 28_800_000;
    private static final int RTL_R820T_I2C_ADDR = 0x34;
    private static final int RTL_R828D_I2C_ADDR = 0x74;
    private static final int RTL_R82XX_CHECK_ADDR = 0x00;
    private static final int RTL_R82XX_CHECK_VAL = 0x69;
    private static final int RTL_R82XX_IF_FREQ = 3_570_000;
    private static final int RTL_R828D_XTAL_HZ = 16_000_000;
    private static final int RTL_R82XX_REG_SHADOW_START = 5;
    private static final int RTL_R82XX_NUM_REGS = 30;
    private static final int RTL_R82XX_CHIP_R820T = 0;
    private static final int RTL_R82XX_CHIP_R828D = 2;
    private static final int[][] RTL_R82XX_FREQ_RANGES = {
            {0, 0x08, 0x02, 0xdf, 0x02, 0x01, 0x00},
            {50, 0x08, 0x02, 0xbe, 0x02, 0x01, 0x00},
            {55, 0x08, 0x02, 0x8b, 0x02, 0x01, 0x00},
            {60, 0x08, 0x02, 0x7b, 0x02, 0x01, 0x00},
            {65, 0x08, 0x02, 0x69, 0x02, 0x01, 0x00},
            {70, 0x08, 0x02, 0x58, 0x02, 0x01, 0x00},
            {75, 0x00, 0x02, 0x44, 0x02, 0x01, 0x00},
            {80, 0x00, 0x02, 0x44, 0x02, 0x01, 0x00},
            {90, 0x00, 0x02, 0x34, 0x01, 0x01, 0x00},
            {100, 0x00, 0x02, 0x34, 0x01, 0x01, 0x00},
            {110, 0x00, 0x02, 0x24, 0x01, 0x01, 0x00},
            {120, 0x00, 0x02, 0x24, 0x01, 0x01, 0x00},
            {140, 0x00, 0x02, 0x14, 0x01, 0x01, 0x00},
            {180, 0x00, 0x02, 0x13, 0x00, 0x00, 0x00},
            {220, 0x00, 0x02, 0x13, 0x00, 0x00, 0x00},
            {250, 0x00, 0x02, 0x11, 0x00, 0x00, 0x00},
            {280, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00},
            {310, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00},
            {450, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00},
            {588, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00},
            {650, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00}
    };
    private static final int[] RTL_FIR_DEFAULT = {
            -54, -36, -41, -40, -32, -14, 14, 53,
            101, 156, 215, 273, 327, 372, 404, 421
    };
    private static final byte[] RTL_R82XX_INIT_ARRAY = {
            (byte) 0x83, 0x32, 0x75,
            (byte) 0xc0, 0x40, (byte) 0xd6, 0x6c,
            (byte) 0xf5, 0x63, 0x75, 0x68,
            0x6c, (byte) 0x83, (byte) 0x80, 0x00,
            0x0f, 0x00, (byte) 0xc0, 0x30,
            0x48, (byte) 0xcc, 0x60, 0x00,
            0x54, (byte) 0xae, 0x4a, (byte) 0xc0
    };
    private static final int FOBOS_INFO_REQUEST = 0xE8;
    private static final int FOBOS_SDR_CMD = 0xE1;
    private static final int CMD_OPEN = 0x00;
    private static final int CMD_CLOSE = 0x01;
    private static final int CMD_START = 0x02;
    private static final int CMD_STOP = 0x03;
    private static final int CMD_SET_FREQ = 0x10;
    private static final int CMD_SET_SR = 0x11;
    private static final int CMD_SET_AUTOBW = 0x13;
    private static final int CMD_SET_DIRECT = 0x20;
    private static final int CMD_SET_LNA = 0x22;
    private static final int CMD_SET_VGA = 0x23;
    private static final int CTRL_TIMEOUT_MS = 300;
    private static final double PREVIEW_MAX_SAMPLE_RATE = 8_000_000.0;
    private static final int PREVIEW_MAX_FFT = 4_096;
    private static final int RTL_PREVIEW_DEFAULT_SAMPLE_RATE = 2_400_000;
    private static final int RTL_PREVIEW_TRANSFER_BYTES = 65_536;
    private static final int RTL_PREVIEW_BUFFER_POOL_DEPTH = 12;
    private static final int RTL_AUDIO_QUEUE_DEPTH = 8;
    private static final int RTL_SPECTRUM_QUEUE_DEPTH = 2;
    private static final long RTL_RETUNE_READ_GRACE_NANOS = 700_000_000L;
    private static final int AUDIO_SAMPLE_RATE = 48_000;
    private static final double AUDIO_RATE_BIAS = 1.0;
    private static final double AUDIO_RATE_MAX_STEP = 0.0035;
    private static final long TELEMETRY_INTERVAL_NANOS = 1_000_000_000L;

    private final Activity activity;
    private final UsbManager usbManager;
    private final Listener listener;
    private final Object previewControlLock = new Object();
    private final PendingIntent permissionIntent;
    private final BroadcastReceiver usbReceiver;
    private boolean receiverRegistered;
    private UsbDevice activeDevice;
    private UsbDeviceConnection activeConnection;
    private UsbInterface activeInterface;
    private UsbEndpoint activeBulkInEndpoint;
    private UsbEndpoint activeBulkOutEndpoint;
    private boolean activeAgileApi;
    private boolean openSessionAfterPermission;
    private volatile boolean previewRunning;
    private Thread previewThread;
    private PreviewRequest pendingPreviewRestart;
    private boolean previewRestartWorkerRunning;
    private volatile boolean previewRestartCancelled;
    private volatile double previewCenterFrequency;
    private volatile double previewListeningFrequency;
    private volatile double previewSampleRate;
    private volatile int previewInputMode;
    private volatile int previewFftLength;
    private volatile int previewRequestedFftLength;
    private volatile double previewBandwidth;
    private volatile int previewModulationType = RadioSettings.MOD_AM;
    private volatile boolean previewAudioEnabled;
    private volatile int previewLnaGain;
    private volatile int previewVgaGain;
    private double audioSourceCursor;
    private double audioMixerPhase;
    private double audioFmLastPhase;
    private double audioFmLastI;
    private double audioFmLastQ;
    private boolean audioFmPhaseValid;
    private double audioEnvelopeAverage = 0.1;
    private double audioDcAverage;
    private double audioLowPassAverage;
    private double audioChannelSumI;
    private double audioChannelSumQ;
    private int audioChannelDecimationCount;
    private double audioChannelLowPassI;
    private double audioChannelLowPassQ;
    private double audioResamplePhase;
    private double audioAgcLevel = 0.05;
    private double[] audioFirTaps = new double[0];
    private double[] audioFirRingI = new double[0];
    private double[] audioFirRingQ = new double[0];
    private int audioFirRingIndex;
    private int audioFirConfiguredDecimation;
    private int audioFirConfiguredModulation;
    private double audioFirConfiguredSampleRate;
    private double audioFirConfiguredBandwidth;
    private double audioFirOutputI;
    private double audioFirOutputQ;
    private double audioDeemphasisAverage;
    private long audioDebugLastLogNanos;
    private double audioRawDcI = 8192.0;
    private double audioRawDcQ = 8192.0;
    private volatile double previewAudioSampleRate;
    private volatile double previewMeasuredSampleRate;
    private long previewAudioRateLastNanos;
    private long previewAudioRateWindowStartNanos;
    private long previewAudioRateWindowSamples;
    private int previewAudioRateAcceptedWindows;
    private int previewAudioRateIgnoredStartupWindows;
    private volatile int previewLastPcmPeak;
    private volatile int previewLastAudioOutputSamples;
    private volatile double previewLastAudioOffsetHz;
    private volatile boolean previewAudioResetRequested;
    private double convertDcI;
    private double convertDcQ;
    private double audioRawRtlDcI = 127.5;
    private double audioRawRtlDcQ = 127.5;
    private volatile int preferredReceiverTarget = USB_TARGET_AUTO;
    private boolean rtlBasebandInitialized;
    private int rtlLastSampleRate;
    private int rtlLastCenterFrequency;
    private int rtlTunerI2cAddress;
    private String rtlTunerName = "";
    private final byte[] rtlR82xxRegs = new byte[RTL_R82XX_NUM_REGS];
    private int rtlR82xxChip = RTL_R82XX_CHIP_R820T;
    private int rtlR82xxXtalHz = RTL_XTAL_HZ;
    private int rtlR82xxIntFreqHz = RTL_R82XX_IF_FREQ;
    private int rtlR82xxInput = -1;
    private boolean rtlR82xxHasLock;
    private volatile long rtlReadTransientUntilNanos;

    private enum ActiveUsbReceiverKind {
        NONE,
        FOBOS,
        RTL_SDR
    }

    private ActiveUsbReceiverKind activeReceiverKind = ActiveUsbReceiverKind.NONE;

    private static final class PreviewRequest {
        final double centerFrequency;
        final double listeningFrequency;
        final double sampleRate;
        final int inputMode;
        final double bandwidth;
        final int modulationType;
        final boolean audioEnabled;
        final int lnaGain;
        final int vgaGain;
        final int fftLength;
        final int requestedFftLength;

        PreviewRequest(double centerFrequency,
                       double listeningFrequency,
                       double sampleRate,
                       int inputMode,
                       double bandwidth,
                       int modulationType,
                       boolean audioEnabled,
                       int lnaGain,
                       int vgaGain,
                       int fftLength,
                       int requestedFftLength) {
            this.centerFrequency = centerFrequency;
            this.listeningFrequency = listeningFrequency;
            this.sampleRate = sampleRate;
            this.inputMode = inputMode;
            this.bandwidth = bandwidth;
            this.modulationType = modulationType;
            this.audioEnabled = audioEnabled;
            this.lnaGain = lnaGain;
            this.vgaGain = vgaGain;
            this.fftLength = fftLength;
            this.requestedFftLength = requestedFftLength;
        }
    }

    private static final class QueuedUsbRead {
        final UsbRequest request;
        final ByteBuffer buffer;
        final byte[] bytes;

        QueuedUsbRead(UsbRequest request, int capacity) {
            this.request = request;
            this.bytes = new byte[capacity];
            this.buffer = ByteBuffer.allocateDirect(capacity);
            this.request.setClientData(this);
        }

        int copyCompletedBytes() {
            int length = Math.max(0, Math.min(buffer.position(), bytes.length));
            buffer.flip();
            buffer.get(bytes, 0, length);
            return length;
        }
    }

    private static final class RtlIqBlock {
        final byte[] bytes;
        final int length;
        final long nanos;
        final int complexSamples;
        final double centerFrequency;
        final double listeningFrequency;
        final double sampleRate;
        final double bandwidth;
        final int modulationType;
        final int fftLength;
        final int requestedFftLength;
        final AtomicInteger references = new AtomicInteger(0);

        RtlIqBlock(byte[] bytes,
                   int length,
                   long nanos,
                   int complexSamples,
                   double centerFrequency,
                   double listeningFrequency,
                   double sampleRate,
                   double bandwidth,
                   int modulationType,
                   int fftLength,
                   int requestedFftLength) {
            this.bytes = bytes;
            this.length = length;
            this.nanos = nanos;
            this.complexSamples = complexSamples;
            this.centerFrequency = centerFrequency;
            this.listeningFrequency = listeningFrequency;
            this.sampleRate = sampleRate;
            this.bandwidth = bandwidth;
            this.modulationType = modulationType;
            this.fftLength = fftLength;
            this.requestedFftLength = requestedFftLength;
        }
    }

    UsbSandbox(Activity activity, Listener listener) {
        this.activity = activity;
        this.listener = listener;
        this.usbManager = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            flags |= PendingIntent.FLAG_MUTABLE;
        }
        this.permissionIntent = PendingIntent.getBroadcast(
                activity,
                0,
                new Intent(ACTION_USB_PERMISSION).setPackage(activity.getPackageName()),
                flags);
        this.usbReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                handleUsbBroadcast(intent);
            }
        };
    }

    // API 33+ uses NOT_EXPORTED below; older Android releases need the legacy overload.
    @android.annotation.SuppressLint("UnspecifiedRegisterReceiverFlag")
    void start() {
        if (receiverRegistered) {
            return;
        }
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            activity.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            activity.registerReceiver(usbReceiver, filter);
        }
        receiverRegistered = true;
    }

    void stop() {
        closeActiveSession();
        if (!receiverRegistered) {
            return;
        }
        try {
            activity.unregisterReceiver(usbReceiver);
        } catch (IllegalArgumentException ignored) {
            // Receiver was already gone during Activity teardown.
        }
        receiverRegistered = false;
    }

    String scanReport() {
        List<UsbDevice> devices = sortedDevices();
        if (devices.isEmpty()) {
            return noUsbDevicesMessage("[USB]");
        }
        StringBuilder report = new StringBuilder();
        report.append("[USB] visible devices: ")
                .append(devices.size())
                .append(", target ")
                .append(receiverTargetLabel(preferredReceiverTarget));
        for (UsbDevice device : devices) {
            appendDeviceReport(report, device);
        }
        return report.toString();
    }

    void setPreferredReceiverTarget(int target) {
        if (target != USB_TARGET_FOBOS && target != USB_TARGET_RTL_SDR) {
            preferredReceiverTarget = USB_TARGET_AUTO;
            return;
        }
        preferredReceiverTarget = target;
    }

    int activeReceiverTarget() {
        if (activeReceiverKind == ActiveUsbReceiverKind.FOBOS) {
            return USB_TARGET_FOBOS;
        }
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
            return USB_TARGET_RTL_SDR;
        }
        return USB_TARGET_AUTO;
    }

    int detectedReceiverTarget() {
        HashMap<String, UsbDevice> deviceList = usbManager.getDeviceList();
        boolean rtlVisible = false;
        for (UsbDevice device : deviceList.values()) {
            if (isFobosCandidate(device)) {
                return USB_TARGET_FOBOS;
            }
            if (isRtlSdrCandidate(device)) {
                rtlVisible = true;
            }
        }
        return rtlVisible ? USB_TARGET_RTL_SDR : USB_TARGET_AUTO;
    }

    String requestPermissionForBestDevice() {
        UsbDevice device = bestDevice();
        if (device == null) {
            return noUsbDevicesMessage("[USB]");
        }
        if (usbManager.hasPermission(device)) {
            return probeOpenDevice(device);
        }
        openSessionAfterPermission = false;
        usbManager.requestPermission(device, permissionIntent);
        return "[USB] permission requested for " + shortDeviceName(device);
    }

    String handleLaunchIntent(Intent intent) {
        if (intent == null || !UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            return "";
        }
        UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        if (device == null) {
            return "[USB] app opened from USB attach intent, but Android did not provide a device";
        }
        StringBuilder report = new StringBuilder();
        report.append("[USB] app opened from attached device: ").append(shortDeviceName(device));
        if (!usbManager.hasPermission(device)) {
            report.append("\n[USB] press USB permission/open or OTG open to grant access");
            return report.toString();
        }
        report.append('\n').append(probeOpenDevice(device));
        return report.toString();
    }

    String openReceiverSession() {
        UsbDevice device = bestDevice();
        if (device == null) {
            return noUsbDevicesMessage("[USB OTG]");
        }
        if (!usbManager.hasPermission(device)) {
            openSessionAfterPermission = true;
            usbManager.requestPermission(device, permissionIntent);
            return "[USB OTG] permission requested for " + shortDeviceName(device);
        }
        return openReceiverSession(device);
    }

    String readReceiverProbe() {
        StringBuilder report = new StringBuilder();
        if (activeConnection == null || activeBulkInEndpoint == null) {
            String openReport = openReceiverSession();
            report.append(openReport);
            if (activeConnection == null || activeBulkInEndpoint == null) {
                return report.toString();
            }
            report.append('\n');
        }
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
            return runRtlSdrBulkProbe(report);
        }
        int maxPacket = Math.max(64, activeBulkInEndpoint.getMaxPacketSize());
        int length = Math.max(512, Math.min(16_384, maxPacket * 16));
        byte[] buffer = new byte[length];
        long startNanos = System.nanoTime();
        int result;
        try {
            result = activeConnection.bulkTransfer(activeBulkInEndpoint, buffer, buffer.length, 50);
        } catch (RuntimeException e) {
            return report.append("[USB OTG] bulk read failed: ").append(e.getMessage()).toString();
        }
        long elapsedMicros = (System.nanoTime() - startNanos) / 1000L;
        report.append("[USB OTG] bulk read probe ep ")
                .append(formatByte(activeBulkInEndpoint.getAddress()))
                .append(" result ").append(result)
                .append(" elapsed ").append(elapsedMicros).append(" us");
        if (result > 0) {
            report.append("\n[USB OTG] first bytes ").append(hexPreview(buffer, result, 32));
        } else {
            report.append("\n[USB OTG] no data is expected until the libfobos init/start sequence is ported");
        }
        return report.toString();
    }

    String readReceiverInfo() {
        StringBuilder report = new StringBuilder();
        if (activeConnection == null) {
            String openReport = openReceiverSession();
            report.append(openReport);
            if (activeConnection == null) {
                return report.toString();
            }
            report.append('\n');
        }
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
            report.append("[USB RTL] device open; firmware strings are not available through Fobos control request");
            return report.toString();
        }
        appendFirmwareInfoProbe(report);
        return report.toString();
    }

    String closeReceiverSession() {
        synchronized (previewControlLock) {
            pendingPreviewRestart = null;
            previewRestartCancelled = true;
        }
        stopPreviewThread(700L);
        boolean hadConnection = activeConnection != null;
        closeActiveSession();
        return hadConnection ? "[USB OTG] session closed" : "[USB OTG] no open session";
    }

    private PreviewRequest makePreviewRequest(double centerFrequency,
                                              double listeningFrequency,
                                              double sampleRate,
                                              int inputMode,
                                              double bandwidth,
                                              int modulationType,
                                              boolean audioEnabled,
                                              int lnaGain,
                                              int vgaGain,
                                              int requestedFftLength) {
        double cappedSampleRate = sampleRate > 0.0
                ? Math.min(sampleRate, PREVIEW_MAX_SAMPLE_RATE)
                : PREVIEW_MAX_SAMPLE_RATE;
        int fftLength = previewFftLength(requestedFftLength);
        int normalizedModulation = normalizeModulationType(modulationType);
        double resolvedBandwidth = bandwidth > 0.0
                ? bandwidth
                : RadioSettings.defaultBandwidthForModulation(normalizedModulation);
        int resolvedRequestedFft = requestedFftLength > 0 ? requestedFftLength : fftLength;
        return new PreviewRequest(centerFrequency,
                listeningFrequency,
                cappedSampleRate,
                inputMode,
                resolvedBandwidth,
                normalizedModulation,
                audioEnabled,
                Math.max(0, Math.min(3, lnaGain)),
                Math.max(0, Math.min(31, vgaGain)),
                fftLength,
                resolvedRequestedFft);
    }

    private PreviewRequest rtlPreviewRequest(PreviewRequest request) {
        double center = request.centerFrequency;
        if (!Double.isFinite(center) || center < 1_000_000.0) {
            center = request.listeningFrequency;
        }
        if (!Double.isFinite(center) || center < 1_000_000.0) {
            center = 100_000_000.0;
        }
        double sampleRate = rtlPreviewSampleRate(request.sampleRate);
        return new PreviewRequest(center,
                request.listeningFrequency,
                sampleRate,
                RadioSettings.INPUT_RF,
                request.bandwidth,
                request.modulationType,
                request.audioEnabled,
                request.lnaGain,
                request.vgaGain,
                request.fftLength,
                request.requestedFftLength);
    }

    private double rtlPreviewSampleRate(double sampleRate) {
        if (!Double.isFinite(sampleRate) || sampleRate <= 0.0) {
            return RTL_PREVIEW_DEFAULT_SAMPLE_RATE;
        }
        int rounded = (int) Math.round(sampleRate);
        if (rounded <= 225_000 ||
                rounded > 3_200_000 ||
                (rounded > 300_000 && rounded <= 900_000)) {
            return RTL_PREVIEW_DEFAULT_SAMPLE_RATE;
        }
        return rounded;
    }

    private int rtlPreviewFrequencyHz(double frequency) {
        if (!Double.isFinite(frequency) || frequency < 1_000_000.0) {
            frequency = 100_000_000.0;
        }
        long rounded = Math.round(frequency);
        rounded = Math.max(24_000_000L, Math.min(1_766_000_000L, rounded));
        return (int) rounded;
    }

    private int rtlGainTenthsFromUi(int lnaGain, int vgaGain) {
        int lna = Math.max(0, Math.min(3, lnaGain));
        int vga = Math.max(0, Math.min(31, vgaGain));
        int target = 120 + lna * 40 + vga * 25;
        return Math.max(0, Math.min(496, target));
    }

    private void applyPreviewRequestState(PreviewRequest request) {
        previewCenterFrequency = request.inputMode == RadioSettings.INPUT_RF
                ? request.centerFrequency
                : 0.0;
        previewListeningFrequency = request.listeningFrequency;
        previewSampleRate = request.sampleRate;
        previewInputMode = request.inputMode;
        previewFftLength = request.fftLength;
        previewRequestedFftLength = request.requestedFftLength;
        previewModulationType = request.modulationType;
        previewBandwidth = request.bandwidth;
        previewAudioEnabled = request.audioEnabled;
        previewLnaGain = request.lnaGain;
        previewVgaGain = request.vgaGain;
    }

    String startReceiverPreview(double centerFrequency,
                                double listeningFrequency,
                                double sampleRate,
                                int inputMode,
                                double bandwidth,
                                int modulationType,
                                boolean audioEnabled,
                                int lnaGain,
                                int vgaGain,
                                int requestedFftLength) {
        PreviewRequest request = makePreviewRequest(centerFrequency,
                listeningFrequency,
                sampleRate,
                inputMode,
                bandwidth,
                modulationType,
                audioEnabled,
                lnaGain,
                vgaGain,
                requestedFftLength);
        synchronized (previewControlLock) {
            previewRestartCancelled = false;
            if (previewRunning || previewThread != null || previewRestartWorkerRunning) {
                pendingPreviewRestart = request;
                if (!previewRestartWorkerRunning) {
                    startPreviewRestartWorkerLocked();
                }
                return "[USB OTG] preview start queued with latest settings";
            }
        }
        return startReceiverPreviewNow(request);
    }

    private String startReceiverPreviewNow(PreviewRequest request) {
        synchronized (previewControlLock) {
            if (previewRunning || previewThread != null) {
                return "[USB OTG] previous preview is still stopping; start deferred";
            }
        }
        StringBuilder report = new StringBuilder();
        if (activeConnection == null || activeBulkInEndpoint == null) {
            String openReport = openReceiverSession();
            report.append(openReport);
            if (activeConnection == null || activeBulkInEndpoint == null) {
                return report.toString();
            }
            report.append('\n');
        }
        PreviewRequest startRequest = activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR
                ? rtlPreviewRequest(request)
                : request;
        synchronized (previewControlLock) {
            applyPreviewRequestState(startRequest);
        }
        resetAudioDemodState();
        resetFobosConversionState();
        previewRunning = true;
        previewThread = new Thread(
                () -> {
                    if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
                        runRtlPreviewLoop(startRequest);
                    } else {
                        runPreviewLoop(startRequest);
                    }
                },
                activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR ? "RtlOtgPreview" : "FobosOtgPreview");
        previewThread.start();
        report.append(activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR ? "[USB RTL]" : "[USB OTG]")
                .append(" preview starting at ")
                .append(formatMhz(startRequest.centerFrequency))
                .append(" MHz, ")
                .append(formatMhz(startRequest.sampleRate))
                .append(" Msps, FFT ")
                .append(startRequest.fftLength);
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR &&
                Math.abs(startRequest.sampleRate - request.sampleRate) > 1.0) {
            report.append("\n[USB RTL] requested sample rate ")
                    .append(formatMhz(request.sampleRate))
                    .append(" Msps is using safe RTL rate ")
                    .append(formatMhz(startRequest.sampleRate))
                    .append(" Msps");
        } else if (startRequest.sampleRate >= PREVIEW_MAX_SAMPLE_RATE) {
            report.append("\n[USB OTG] preview sample rate capped for first Android streaming tests");
        }
        return report.toString();
    }

    String stopReceiverPreview() {
        synchronized (previewControlLock) {
            pendingPreviewRestart = null;
            previewRestartCancelled = true;
        }
        boolean wasRunning = previewRunning || previewThread != null;
        stopPreviewThread(1000L);
        return wasRunning ? "[USB OTG] preview stop requested" : "[USB OTG] preview is not running";
    }

    boolean isReceiverPreviewRunning() {
        return previewRunning;
    }

    String applyReceiverPreviewSettings(double centerFrequency,
                                        double listeningFrequency,
                                        double sampleRate,
                                        int inputMode,
                                        double bandwidth,
                                        int modulationType,
                                        boolean audioEnabled,
                                        int lnaGain,
                                        int vgaGain,
                                        int requestedFftLength) {
        if (!previewRunning || activeConnection == null) {
            return "[USB OTG] preview is not running";
        }
        PreviewRequest request = makePreviewRequest(centerFrequency,
                listeningFrequency,
                sampleRate,
                inputMode,
                bandwidth,
                modulationType,
                audioEnabled,
                lnaGain,
                vgaGain,
                requestedFftLength);
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
            return applyRtlReceiverPreviewSettings(request);
        }
        StringBuilder report = new StringBuilder("[USB OTG] preview settings");
        try {
            boolean audioResetNeeded;
            boolean needsRestart;
            synchronized (previewControlLock) {
                audioResetNeeded = Math.abs(request.sampleRate - previewSampleRate) > 1.0 ||
                        request.inputMode != previewInputMode ||
                        request.modulationType != previewModulationType ||
                        Math.abs(request.listeningFrequency - previewListeningFrequency) > 10.0 ||
                        Math.abs(request.centerFrequency - previewCenterFrequency) > 10.0;
                needsRestart = Math.abs(request.sampleRate - previewSampleRate) > 1.0 ||
                        request.inputMode != previewInputMode ||
                        request.fftLength != previewFftLength;

                if (needsRestart) {
                    previewRestartCancelled = false;
                    pendingPreviewRestart = request;
                    if (!previewRestartWorkerRunning) {
                        startPreviewRestartWorkerLocked();
                    }
                    return report.append(" queued restart for sample/input/FFT change").toString();
                }

                applyPreviewRequestState(request);
            }

            int lnaResult = fx3Command(CMD_SET_LNA, request.lnaGain);
            int vgaResult = fx3Command(CMD_SET_VGA, request.vgaGain);
            report.append(" LNA ").append(lnaResult).append(", VGA ").append(vgaResult);

            if (request.inputMode == RadioSettings.INPUT_RF) {
                long frequency = Math.max(50_000_000L, Math.round(request.centerFrequency));
                int freqResult = controlOutU64(CMD_SET_FREQ, frequency);
                previewCenterFrequency = frequency;
                previewListeningFrequency = request.listeningFrequency;
                report.append(", RF ").append(formatMhz(frequency)).append(" MHz ").append(freqResult);
            } else {
                previewCenterFrequency = 0.0;
                previewListeningFrequency = request.listeningFrequency;
                report.append(", direct input frequency scale only");
            }
            if (audioResetNeeded) {
                previewAudioResetRequested = true;
            }
        } catch (RuntimeException e) {
            report.append(" failed: ").append(e.getMessage());
        }
        return report.toString();
    }

    private String applyRtlReceiverPreviewSettings(PreviewRequest incomingRequest) {
        PreviewRequest request = rtlPreviewRequest(incomingRequest);
        StringBuilder report = new StringBuilder("[USB RTL] preview settings");
        try {
            boolean audioResetNeeded;
            boolean needsRestart;
            boolean gainChanged;
            synchronized (previewControlLock) {
                audioResetNeeded = Math.abs(request.sampleRate - previewSampleRate) > 1.0 ||
                        request.modulationType != previewModulationType ||
                        Math.abs(request.listeningFrequency - previewListeningFrequency) > 10.0 ||
                        Math.abs(request.centerFrequency - previewCenterFrequency) > 10.0;
                needsRestart = Math.abs(request.sampleRate - previewSampleRate) > 1.0 ||
                        request.fftLength != previewFftLength;
                gainChanged = request.lnaGain != previewLnaGain ||
                        request.vgaGain != previewVgaGain;

                if (needsRestart) {
                    previewRestartCancelled = false;
                    pendingPreviewRestart = request;
                    if (!previewRestartWorkerRunning) {
                        startPreviewRestartWorkerLocked();
                    }
                    return report.append(" queued restart for sample/FFT change").toString();
                }

                applyPreviewRequestState(request);
            }

            int centerHz = rtlPreviewFrequencyHz(request.centerFrequency);
            Log.d(LOG_TAG, String.format(Locale.US,
                    "RTL apply settings reqCenter=%.3fMHz previewCenter=%.3fMHz centerHz=%.3fMHz last=%s tuner=0x%02x sample=%.3fMHz fft=%d listen=%.3fMHz audio=%s gainChanged=%s",
                    request.centerFrequency / 1_000_000.0,
                    previewCenterFrequency / 1_000_000.0,
                    centerHz / 1_000_000.0,
                    rtlLastCenterFrequency > 0
                            ? String.format(Locale.US, "%.3fMHz", rtlLastCenterFrequency / 1_000_000.0)
                            : "none",
                    rtlTunerI2cAddress,
                    request.sampleRate / 1_000_000.0,
                    request.fftLength,
                    request.listeningFrequency / 1_000_000.0,
                    request.audioEnabled,
                    gainChanged));
            if (rtlTunerI2cAddress != 0 &&
                    (Math.abs(centerHz - rtlLastCenterFrequency) > 10 || gainChanged)) {
                StringBuilder tuneReport = new StringBuilder();
                rtlReadTransientUntilNanos = System.nanoTime() + RTL_RETUNE_READ_GRACE_NANOS;
                rtlSetI2cRepeater(true, tuneReport);
                boolean tuned = true;
                if (Math.abs(centerHz - rtlLastCenterFrequency) > 10) {
                    tuned = rtlR82xxSetFrequency(centerHz, tuneReport);
                }
                boolean gainOk = true;
                if (gainChanged) {
                    gainOk = rtlR82xxSetGain(rtlGainTenthsFromUi(request.lnaGain, request.vgaGain),
                            tuneReport);
                }
                rtlSetI2cRepeater(false, tuneReport);
                if (tuned) {
                    rtlLastCenterFrequency = centerHz;
                    rtlResetBuffer(tuneReport);
                    rtlReadTransientUntilNanos = System.nanoTime() + RTL_RETUNE_READ_GRACE_NANOS;
                }
                log(tuneReport.toString());
                report.append(", tune ")
                        .append(formatMhz(centerHz))
                        .append(" MHz ")
                        .append(tuned ? "ok" : "failed");
                if (gainChanged) {
                    report.append(", gain ").append(gainOk ? "ok" : "failed");
                }
            } else {
                report.append(", frequency scale only");
                Log.d(LOG_TAG, String.format(Locale.US,
                        "RTL retune skipped centerHz=%.3fMHz last=%s tuner=0x%02x gainChanged=%s",
                        centerHz / 1_000_000.0,
                        rtlLastCenterFrequency > 0
                                ? String.format(Locale.US, "%.3fMHz", rtlLastCenterFrequency / 1_000_000.0)
                                : "none",
                        rtlTunerI2cAddress,
                        gainChanged));
            }
            if (audioResetNeeded) {
                previewAudioResetRequested = true;
            }
        } catch (RuntimeException e) {
            report.append(" failed: ").append(e.getMessage());
        }
        return report.toString();
    }

    String runReceiverSampleTest() {
        StringBuilder report = new StringBuilder();
        if (activeConnection == null || activeBulkInEndpoint == null) {
            String openReport = openReceiverSession();
            report.append(openReport);
            if (activeConnection == null || activeBulkInEndpoint == null) {
                return report.toString();
            }
            report.append('\n');
        }
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
            return runRtlSdrBulkProbe(report);
        }

        boolean openedByCommand = false;
        boolean started = false;
        try {
            int result = fx3Command(CMD_OPEN, 0);
            openedByCommand = result >= 0;
            report.append("[USB OTG] CMD_OPEN result ").append(result);

            result = controlOutU64(CMD_SET_FREQ, 100_000_000L);
            report.append("\n[USB OTG] SET_FREQ 100 MHz result ").append(result);

            result = controlOutU64(CMD_SET_SR, 8_000_000L);
            report.append("\n[USB OTG] SET_SR 8 Msps result ").append(result);

            result = controlOutU64(CMD_SET_AUTOBW, Math.round(0.9 * 1024.0));
            report.append("\n[USB OTG] SET_AUTOBW 0.9 result ").append(result);

            int packsPerTransfer = 16;
            result = fx3Command(CMD_START, packsPerTransfer);
            started = result >= 0;
            report.append("\n[USB OTG] CMD_START packs ")
                    .append(packsPerTransfer)
                    .append(" result ")
                    .append(result);

            byte[] buffer = new byte[65_536];
            long startNanos = System.nanoTime();
            result = activeConnection.bulkTransfer(activeBulkInEndpoint, buffer, buffer.length, 1000);
            long elapsedMicros = (System.nanoTime() - startNanos) / 1000L;
            report.append("\n[USB OTG] sample bulk read result ")
                    .append(result)
                    .append(" elapsed ")
                    .append(elapsedMicros)
                    .append(" us");
            if (result > 0) {
                report.append("\n[USB OTG] sample first bytes ")
                        .append(hexPreview(buffer, result, 32));
            }
        } catch (RuntimeException e) {
            report.append("\n[USB OTG] sample test failed: ").append(e.getMessage());
        } finally {
            if (started) {
                int stopResult = fx3Command(CMD_STOP, 0);
                report.append("\n[USB OTG] CMD_STOP result ").append(stopResult);
            }
            if (openedByCommand) {
                int closeResult = fx3Command(CMD_CLOSE, 0);
                report.append("\n[USB OTG] CMD_CLOSE result ").append(closeResult);
            }
        }
        return report.toString();
    }

    private String runRtlSdrBulkProbe(StringBuilder report) {
        if (activeConnection == null || activeBulkInEndpoint == null) {
            return report.append("[USB RTL] no open bulk IN endpoint").toString();
        }
        if (!rtlEnsureInitialized(report, 100_000_000, 2_400_000)) {
            report.append("\n[USB RTL] init failed; bulk probe skipped");
            return report.toString();
        }
        byte[] buffer = new byte[16_384];
        long startNanos = System.nanoTime();
        int result;
        try {
            result = activeConnection.bulkTransfer(activeBulkInEndpoint, buffer, buffer.length, 150);
        } catch (RuntimeException e) {
            return report.append("[USB RTL] bulk probe failed: ").append(e.getMessage()).toString();
        }
        long elapsedMicros = (System.nanoTime() - startNanos) / 1000L;
        report.append("[USB RTL] bulk probe endpoint ")
                .append(endpointSummary(activeBulkInEndpoint))
                .append(" result ")
                .append(result)
                .append(" elapsed ")
                .append(elapsedMicros)
                .append(" us");
        if (result > 0) {
            report.append("\n[USB RTL] first bytes ").append(hexPreview(buffer, result, 32));
        } else {
            report.append("\n[USB RTL] no IQ after init; full R82xx PLL init may be required");
        }
        return report.toString();
    }

    private boolean rtlEnsureInitialized(StringBuilder report, int centerFrequencyHz, int sampleRateHz) {
        if (activeConnection == null) {
            report.append("[USB RTL] no active USB connection");
            return false;
        }
        int safeSampleRate = sampleRateHz;
        if (safeSampleRate <= 225_000 || safeSampleRate > 3_200_000 ||
                (safeSampleRate > 300_000 && safeSampleRate <= 900_000)) {
            safeSampleRate = 2_400_000;
        }

        if (!rtlBasebandInitialized) {
            report.append("[USB RTL] init begin center ")
                    .append(centerFrequencyHz)
                    .append(" sampleRate ")
                    .append(safeSampleRate);
            if (!rtlInitBaseband(report)) {
                return false;
            }
            if (!rtlProbeAndPrepareTuner(report)) {
                report.append("\n[USB RTL] tuner probe did not find R820T/R828D; continuing baseband-only");
            }
            rtlBasebandInitialized = true;
        }

        if (rtlLastSampleRate != safeSampleRate) {
            if (!rtlSetSampleRate(safeSampleRate, report)) {
                return false;
            }
            rtlLastSampleRate = safeSampleRate;
        }
        if (rtlTunerI2cAddress != 0 && rtlLastCenterFrequency != centerFrequencyHz) {
            rtlSetI2cRepeater(true, report);
            boolean tuned = rtlR82xxSetFrequency(centerFrequencyHz, report);
            rtlSetI2cRepeater(false, report);
            if (!tuned) {
                return false;
            }
            rtlLastCenterFrequency = centerFrequencyHz;
        }
        if (!rtlResetBuffer(report)) {
            return false;
        }
        return true;
    }

    private boolean rtlInitBaseband(StringBuilder report) {
        boolean ok = true;
        ok &= rtlWriteReg(RTL_BLOCK_USB, RTL_USB_SYSCTL, 0x09, 1, "USB_SYSCTL", report);
        ok &= rtlWriteReg(RTL_BLOCK_USB, RTL_USB_EPA_MAXPKT, 0x0002, 2, "USB_EPA_MAXPKT", report);
        ok &= rtlWriteReg(RTL_BLOCK_USB, RTL_USB_EPA_CTL, 0x1002, 2, "USB_EPA_CTL init", report);
        ok &= rtlWriteReg(RTL_BLOCK_SYS, RTL_DEMOD_CTL_1, 0x22, 1, "DEMOD_CTL_1", report);
        ok &= rtlWriteReg(RTL_BLOCK_SYS, RTL_DEMOD_CTL, 0xe8, 1, "DEMOD_CTL power", report);
        ok &= rtlDemodWriteReg(1, 0x01, 0x14, 1, "demod soft reset 1", report);
        ok &= rtlDemodWriteReg(1, 0x01, 0x10, 1, "demod soft reset 2", report);
        ok &= rtlDemodWriteReg(1, 0x15, 0x00, 1, "disable inversion", report);
        ok &= rtlDemodWriteReg(1, 0x16, 0x0000, 2, "disable ACR", report);
        for (int i = 0; i < 6; ++i) {
            ok &= rtlDemodWriteReg(1, 0x16 + i, 0x00, 1, "clear DDC/IF " + i, report);
        }
        ok &= rtlSetFir(report);
        ok &= rtlDemodWriteReg(0, 0x19, 0x05, 1, "SDR mode", report);
        ok &= rtlDemodWriteReg(1, 0x93, 0xf0, 1, "FSM 0x93", report);
        ok &= rtlDemodWriteReg(1, 0x94, 0x0f, 1, "FSM 0x94", report);
        ok &= rtlDemodWriteReg(1, 0x11, 0x00, 1, "DAGC off", report);
        ok &= rtlDemodWriteReg(1, 0x04, 0x00, 1, "RF/IF AGC off", report);
        ok &= rtlDemodWriteReg(0, 0x61, 0x60, 1, "PID filter off", report);
        ok &= rtlDemodWriteReg(0, 0x06, 0x80, 1, "ADC IQ path", report);
        ok &= rtlDemodWriteReg(1, 0xb1, 0x1b, 1, "zero IF mode", report);
        ok &= rtlDemodWriteReg(0, 0x0d, 0x83, 1, "clock output off", report);
        report.append("\n[USB RTL] baseband init ").append(ok ? "ok" : "failed");
        return ok;
    }

    private boolean rtlProbeAndPrepareTuner(StringBuilder report) {
        rtlSetI2cRepeater(true, report);
        int r820Check = rtlI2cReadReg(RTL_R820T_I2C_ADDR, RTL_R82XX_CHECK_ADDR, "R820T probe", report);
        int r828Check = rtlI2cReadReg(RTL_R828D_I2C_ADDR, RTL_R82XX_CHECK_ADDR, "R828D probe", report);
        if (r820Check == RTL_R82XX_CHECK_VAL) {
            rtlTunerI2cAddress = RTL_R820T_I2C_ADDR;
            rtlTunerName = "R820T";
            rtlR82xxChip = RTL_R82XX_CHIP_R820T;
            rtlR82xxXtalHz = RTL_XTAL_HZ;
        } else if (r828Check == RTL_R82XX_CHECK_VAL) {
            rtlTunerI2cAddress = RTL_R828D_I2C_ADDR;
            rtlTunerName = "R828D";
            rtlR82xxChip = RTL_R82XX_CHIP_R828D;
            rtlR82xxXtalHz = RTL_R828D_XTAL_HZ;
        } else {
            rtlSetI2cRepeater(false, report);
            report.append("\n[USB RTL] R82xx probe miss R820T=0x")
                    .append(Integer.toHexString(r820Check & 0xff))
                    .append(" R828D=0x")
                    .append(Integer.toHexString(r828Check & 0xff));
            return false;
        }

        report.append("\n[USB RTL] tuner ").append(rtlTunerName)
                .append(" detected at 0x")
                .append(Integer.toHexString(rtlTunerI2cAddress));

        boolean ok = true;
        ok &= rtlDemodWriteReg(1, 0xb1, 0x1a, 1, "R82xx zero IF off", report);
        ok &= rtlDemodWriteReg(0, 0x08, 0x4d, 1, "R82xx I ADC only", report);
        ok &= rtlSetIfFrequency(RTL_R82XX_IF_FREQ, report);
        ok &= rtlDemodWriteReg(1, 0x15, 0x01, 1, "R82xx spectrum inversion", report);
        ok &= rtlR82xxWriteInitialRegisters(report);
        ok &= rtlR82xxSetGain(240, report);
        rtlSetI2cRepeater(false, report);
        report.append("\n[USB RTL] tuner prep ").append(ok ? "ok" : "failed");
        return ok;
    }

    private boolean rtlR82xxWriteInitialRegisters(StringBuilder report) {
        if (rtlTunerI2cAddress == 0) {
            return false;
        }
        System.arraycopy(RTL_R82XX_INIT_ARRAY, 0, rtlR82xxRegs, 0, RTL_R82XX_INIT_ARRAY.length);
        int pos = 0;
        int reg = 0x05;
        boolean ok = true;
        while (pos < RTL_R82XX_INIT_ARRAY.length) {
            int chunk = Math.min(7, RTL_R82XX_INIT_ARRAY.length - pos);
            byte[] payload = new byte[chunk];
            System.arraycopy(RTL_R82XX_INIT_ARRAY, pos, payload, 0, chunk);
            ok &= rtlR82xxWrite(reg, payload, chunk,
                    "R82xx init 0x" + Integer.toHexString(reg), report);
            pos += chunk;
            reg += chunk;
        }
        return ok;
    }

    private boolean rtlR82xxSetFrequency(int frequencyHz, StringBuilder report) {
        if (rtlTunerI2cAddress == 0) {
            report.append("\n[USB RTL] set frequency skipped: no R82xx tuner");
            return false;
        }
        int loFrequencyHz = frequencyHz + rtlR82xxIntFreqHz;
        boolean ok = rtlR82xxSetMux(loFrequencyHz, report);
        ok &= rtlR82xxSetPll(loFrequencyHz, report);
        if (rtlR82xxChip == RTL_R82XX_CHIP_R828D) {
            int input = frequencyHz > 345_000_000 ? 0x00 : 0x60;
            if (input != rtlR82xxInput) {
                ok &= rtlR82xxWriteRegMask(0x05, input, 0x60, "R828D input", report);
                rtlR82xxInput = input;
            }
        }
        report.append("\n[USB RTL] tune ")
                .append(frequencyHz)
                .append(" Hz LO ")
                .append(loFrequencyHz)
                .append(' ')
                .append(ok ? "ok" : "failed")
                .append(" lock ")
                .append(rtlR82xxHasLock);
        return ok;
    }

    private boolean rtlR82xxSetMux(int frequencyHz, StringBuilder report) {
        int frequencyMhz = Math.max(0, frequencyHz / 1_000_000);
        int[] range = RTL_R82XX_FREQ_RANGES[RTL_R82XX_FREQ_RANGES.length - 1];
        for (int i = 0; i < RTL_R82XX_FREQ_RANGES.length - 1; ++i) {
            if (frequencyMhz < RTL_R82XX_FREQ_RANGES[i + 1][0]) {
                range = RTL_R82XX_FREQ_RANGES[i];
                break;
            }
        }
        boolean ok = true;
        ok &= rtlR82xxWriteRegMask(0x17, range[1], 0x08, "R82xx open drain", report);
        ok &= rtlR82xxWriteRegMask(0x1a, range[2], 0xc3, "R82xx RF mux", report);
        ok &= rtlR82xxWriteReg(0x1b, range[3], "R82xx TF band", report);
        int xtalCap = range[6] | 0x00; // librtlsdr uses XTAL_HIGH_CAP_0P for R82xx init.
        ok &= rtlR82xxWriteRegMask(0x10, xtalCap, 0x0b, "R82xx xtal cap", report);
        ok &= rtlR82xxWriteRegMask(0x08, 0x00, 0x3f, "R82xx image gain", report);
        ok &= rtlR82xxWriteRegMask(0x09, 0x00, 0x3f, "R82xx image phase", report);
        return ok;
    }

    private boolean rtlR82xxSetPll(int frequencyHz, StringBuilder report) {
        final int vcoMinKhz = 1_770_000;
        final int vcoMaxKhz = vcoMinKhz * 2;
        int frequencyKhz = (frequencyHz + 500) / 1000;
        int pllRef = rtlR82xxXtalHz;

        boolean ok = rtlR82xxWriteRegMask(0x1a, 0x00, 0x0c, "R82xx PLL autotune 128k", report);
        byte[] regs = new byte[7];
        for (int i = 0; i < regs.length; ++i) {
            regs[i] = rtlR82xxRegs[(0x10 - RTL_R82XX_REG_SHADOW_START) + i];
        }
        regs[0] = (byte) maskReg8(regs[0] & 0xff, 0x00, 0x10);
        regs[2] = (byte) maskReg8(regs[2] & 0xff, 0x80, 0xe0);

        int mixDiv = 2;
        int divNum = 0;
        while (mixDiv <= 64) {
            long mixedKhz = (long) frequencyKhz * mixDiv;
            if (mixedKhz >= vcoMinKhz && mixedKhz < vcoMaxKhz) {
                int divBuf = mixDiv;
                while (divBuf > 2) {
                    divBuf >>= 1;
                    divNum++;
                }
                break;
            }
            mixDiv <<= 1;
        }
        if (mixDiv > 64) {
            report.append("\n[USB RTL] R82xx PLL no divider for ").append(frequencyHz);
            return false;
        }

        byte[] read = new byte[5];
        if (!rtlR82xxRead(0x00, read, read.length, "R82xx PLL read", report)) {
            return false;
        }
        int vcoPowerRef = rtlR82xxChip == RTL_R82XX_CHIP_R828D ? 1 : 2;
        int vcoFineTune = (read[4] & 0x30) >> 4;
        if (vcoFineTune > vcoPowerRef) {
            divNum--;
        } else if (vcoFineTune < vcoPowerRef) {
            divNum++;
        }
        regs[0] = (byte) maskReg8(regs[0] & 0xff, divNum << 5, 0xe0);

        long vcoFrequency = (long) frequencyHz * mixDiv;
        long vcoDiv = (pllRef + 65_536L * vcoFrequency) / (2L * pllRef);
        int nint = (int) (vcoDiv / 65_536L);
        int sdm = (int) (vcoDiv % 65_536L);
        if (nint > ((128 / vcoPowerRef) - 1)) {
            report.append("\n[USB RTL] R82xx PLL invalid nint ").append(nint);
            return false;
        }
        int ni = (nint - 13) / 4;
        int si = nint - 4 * ni - 13;
        regs[4] = (byte) (ni + (si << 6));
        regs[2] = (byte) maskReg8(regs[2] & 0xff, sdm == 0 ? 0x08 : 0x00, 0x08);
        regs[5] = (byte) (sdm & 0xff);
        regs[6] = (byte) ((sdm >> 8) & 0xff);

        ok &= rtlR82xxWrite(0x10, regs, regs.length, "R82xx PLL regs", report);
        rtlR82xxHasLock = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            sleepQuietly(10);
            byte[] lockData = new byte[3];
            if (!rtlR82xxRead(0x00, lockData, lockData.length, "R82xx PLL lock", report)) {
                return false;
            }
            if ((lockData[2] & 0x40) != 0) {
                rtlR82xxHasLock = true;
                break;
            }
            if (attempt == 0) {
                ok &= rtlR82xxWriteRegMask(0x12, 0x60, 0xe0, "R82xx VCO current", report);
            }
        }
        ok &= rtlR82xxWriteRegMask(0x1a, 0x08, 0x08, "R82xx PLL autotune 8k", report);
        return ok;
    }

    private boolean rtlR82xxSetGain(int gainTenthsDb, StringBuilder report) {
        final int[] lnaSteps = {
                0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13
        };
        final int[] mixerSteps = {
                0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8
        };
        int total = 0;
        int lna = 0;
        int mixer = 0;
        for (int i = 0; i < 15; ++i) {
            if (total >= gainTenthsDb) {
                break;
            }
            total += lnaSteps[++lna];
            if (total >= gainTenthsDb) {
                break;
            }
            total += mixerSteps[++mixer];
        }
        boolean ok = true;
        ok &= rtlR82xxWriteRegMask(0x05, 0x10, 0x10, "R82xx LNA manual", report);
        ok &= rtlR82xxWriteRegMask(0x07, 0x00, 0x10, "R82xx mixer manual", report);
        ok &= rtlR82xxWriteRegMask(0x0c, 0x08, 0x9f, "R82xx VGA fixed", report);
        ok &= rtlR82xxWriteRegMask(0x05, lna, 0x0f, "R82xx LNA gain", report);
        ok &= rtlR82xxWriteRegMask(0x07, mixer, 0x0f, "R82xx mixer gain", report);
        report.append("\n[USB RTL] gain target ")
                .append(gainTenthsDb)
                .append(" actual-ish ")
                .append(total)
                .append(" lna ")
                .append(lna)
                .append(" mixer ")
                .append(mixer)
                .append(' ')
                .append(ok ? "ok" : "failed");
        return ok;
    }

    private boolean rtlR82xxWriteReg(int register, int value, String label, StringBuilder report) {
        byte[] payload = {(byte) (value & 0xff)};
        return rtlR82xxWrite(register, payload, 1, label, report);
    }

    private boolean rtlR82xxWriteRegMask(int register, int value, int mask,
                                         String label, StringBuilder report) {
        int current = rtlR82xxReadCacheReg(register);
        if (current < 0) {
            report.append("\n[USB RTL] ").append(label).append(" cache miss 0x")
                    .append(Integer.toHexString(register));
            return false;
        }
        int next = (current & ~mask) | (value & mask);
        return rtlR82xxWriteReg(register, next, label, report);
    }

    private boolean rtlR82xxWrite(int register, byte[] values, int length,
                                  String label, StringBuilder report) {
        if (rtlTunerI2cAddress == 0) {
            return false;
        }
        int pos = 0;
        int reg = register;
        boolean ok = true;
        while (pos < length) {
            int chunk = Math.min(7, length - pos);
            byte[] payload = new byte[chunk + 1];
            payload[0] = (byte) (reg & 0xff);
            System.arraycopy(values, pos, payload, 1, chunk);
            ok &= rtlWriteArray(RTL_BLOCK_I2C, rtlTunerI2cAddress, payload, payload.length,
                    label, report);
            if (ok) {
                rtlR82xxShadowStore(reg, values, pos, chunk);
            }
            reg += chunk;
            pos += chunk;
        }
        return ok;
    }

    private boolean rtlR82xxRead(int register, byte[] values, int length,
                                 String label, StringBuilder report) {
        byte[] reg = {(byte) (register & 0xff)};
        if (!rtlWriteArray(RTL_BLOCK_I2C, rtlTunerI2cAddress, reg, 1, label + " addr", report)) {
            return false;
        }
        byte[] raw = new byte[length];
        int result = rtlReadArray(RTL_BLOCK_I2C, rtlTunerI2cAddress, raw, length, label, report);
        if (result != length) {
            return false;
        }
        for (int i = 0; i < length; ++i) {
            values[i] = (byte) bitReverse8(raw[i] & 0xff);
        }
        return true;
    }

    private void rtlR82xxShadowStore(int register, byte[] values, int offset, int length) {
        int shadow = register - RTL_R82XX_REG_SHADOW_START;
        int src = offset;
        int len = length;
        if (shadow < 0) {
            src -= shadow;
            len += shadow;
            shadow = 0;
        }
        if (len <= 0 || shadow >= rtlR82xxRegs.length) {
            return;
        }
        len = Math.min(len, rtlR82xxRegs.length - shadow);
        System.arraycopy(values, src, rtlR82xxRegs, shadow, len);
    }

    private int rtlR82xxReadCacheReg(int register) {
        int shadow = register - RTL_R82XX_REG_SHADOW_START;
        if (shadow < 0 || shadow >= rtlR82xxRegs.length) {
            return -1;
        }
        return rtlR82xxRegs[shadow] & 0xff;
    }

    private int bitReverse8(int value) {
        int low = value & 0x0f;
        int high = (value >> 4) & 0x0f;
        final int[] lut = {0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
                0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf};
        return (lut[low] << 4) | lut[high];
    }

    private int maskReg8(int register, int value, int mask) {
        return (register & ~mask) | (value & mask);
    }

    private boolean rtlSetIfFrequency(int ifHz, StringBuilder report) {
        long ifFreq = -((long) ifHz * (1L << 22) / RTL_XTAL_HZ);
        boolean ok = true;
        ok &= rtlDemodWriteReg(1, 0x19, (int) ((ifFreq >> 16) & 0x3f), 1, "IF freq hi", report);
        ok &= rtlDemodWriteReg(1, 0x1a, (int) ((ifFreq >> 8) & 0xff), 1, "IF freq mid", report);
        ok &= rtlDemodWriteReg(1, 0x1b, (int) (ifFreq & 0xff), 1, "IF freq lo", report);
        return ok;
    }

    private boolean rtlSetSampleRate(int sampleRateHz, StringBuilder report) {
        long ratio = ((long) RTL_XTAL_HZ * (1L << 22)) / sampleRateHz;
        ratio &= 0x0ffffffcL;
        long deviceRatio = ratio | ((ratio & 0x08000000L) << 1);
        boolean ok = true;
        ok &= rtlDemodWriteReg(1, 0x9f, (int) ((deviceRatio >> 16) & 0xffff), 2,
                "sample ratio hi", report);
        ok &= rtlDemodWriteReg(1, 0xa1, (int) (deviceRatio & 0xffff), 2,
                "sample ratio lo", report);
        ok &= rtlSetSampleFrequencyCorrection(0, report);
        ok &= rtlDemodWriteReg(1, 0x01, 0x14, 1, "sample reset 1", report);
        ok &= rtlDemodWriteReg(1, 0x01, 0x10, 1, "sample reset 2", report);
        report.append("\n[USB RTL] sample rate ")
                .append(sampleRateHz)
                .append(" ratio 0x")
                .append(Long.toHexString(deviceRatio))
                .append(' ')
                .append(ok ? "ok" : "failed");
        return ok;
    }

    private boolean rtlSetSampleFrequencyCorrection(int ppm, StringBuilder report) {
        int offs = (int) ((long) ppm * -1L * (1L << 24) / 1_000_000L);
        boolean ok = true;
        ok &= rtlDemodWriteReg(1, 0x3f, offs & 0xff, 1, "ppm corr lo", report);
        ok &= rtlDemodWriteReg(1, 0x3e, (offs >> 8) & 0x3f, 1, "ppm corr hi", report);
        return ok;
    }

    private boolean rtlResetBuffer(StringBuilder report) {
        boolean ok = true;
        ok &= rtlWriteReg(RTL_BLOCK_USB, RTL_USB_EPA_CTL, 0x1002, 2, "EPA reset begin", report);
        ok &= rtlWriteReg(RTL_BLOCK_USB, RTL_USB_EPA_CTL, 0x0000, 2, "EPA reset end", report);
        report.append("\n[USB RTL] buffer reset ").append(ok ? "ok" : "failed");
        return ok;
    }

    private boolean rtlSetFir(StringBuilder report) {
        byte[] fir = new byte[20];
        for (int i = 0; i < 8; ++i) {
            fir[i] = (byte) RTL_FIR_DEFAULT[i];
        }
        for (int i = 0; i < 8; i += 2) {
            int val0 = RTL_FIR_DEFAULT[8 + i];
            int val1 = RTL_FIR_DEFAULT[8 + i + 1];
            int out = 8 + i * 3 / 2;
            fir[out] = (byte) (val0 >> 4);
            fir[out + 1] = (byte) ((val0 << 4) | ((val1 >> 8) & 0x0f));
            fir[out + 2] = (byte) val1;
        }
        boolean ok = true;
        for (int i = 0; i < fir.length; ++i) {
            ok &= rtlDemodWriteReg(1, 0x1c + i, fir[i] & 0xff, 1, "FIR " + i, report);
        }
        return ok;
    }

    private void rtlSetI2cRepeater(boolean enabled, StringBuilder report) {
        rtlDemodWriteReg(1, 0x01, enabled ? 0x18 : 0x10, 1,
                enabled ? "I2C repeater on" : "I2C repeater off", report);
    }

    private int rtlI2cReadReg(int i2cAddress, int register, String label, StringBuilder report) {
        byte[] reg = {(byte) (register & 0xff)};
        if (!rtlWriteArray(RTL_BLOCK_I2C, i2cAddress, reg, 1, label + " addr", report)) {
            return -1;
        }
        byte[] data = new byte[1];
        int result = rtlReadArray(RTL_BLOCK_I2C, i2cAddress, data, 1, label, report);
        return result == 1 ? data[0] & 0xff : -1;
    }

    private boolean rtlWriteReg(int block, int address, int value, int length,
                                String label, StringBuilder report) {
        byte[] data = new byte[Math.max(1, length)];
        if (length == 1) {
            data[0] = (byte) (value & 0xff);
        } else {
            data[0] = (byte) ((value >> 8) & 0xff);
            data[1] = (byte) (value & 0xff);
        }
        return rtlWriteArray(block, address, data, length, label, report);
    }

    private int rtlReadReg(int block, int address, int length, String label, StringBuilder report) {
        byte[] data = new byte[Math.max(1, length)];
        int result = rtlReadArray(block, address, data, length, label, report);
        if (result < 0) {
            return result;
        }
        if (length == 1) {
            return data[0] & 0xff;
        }
        return ((data[1] & 0xff) << 8) | (data[0] & 0xff);
    }

    private boolean rtlDemodWriteReg(int page, int address, int value, int length,
                                     String label, StringBuilder report) {
        byte[] data = new byte[Math.max(1, length)];
        if (length == 1) {
            data[0] = (byte) (value & 0xff);
        } else {
            data[0] = (byte) ((value >> 8) & 0xff);
            data[1] = (byte) (value & 0xff);
        }
        int requestValue = ((address & 0xffff) << 8) | 0x20;
        int requestIndex = 0x10 | (page & 0xff);
        int result = rtlControlOut(requestValue, requestIndex, data, length);
        if (result != length) {
            report.append("\n[USB RTL] ").append(label)
                    .append(" failed result ").append(result);
            return false;
        }
        rtlDemodReadReg(0x0a, 0x01, 1, label + " latch", null);
        return true;
    }

    private int rtlDemodReadReg(int page, int address, int length,
                                String label, StringBuilder report) {
        byte[] data = new byte[Math.max(1, length)];
        int requestValue = ((address & 0xffff) << 8) | 0x20;
        int requestIndex = page & 0xff;
        int result = rtlControlIn(requestValue, requestIndex, data, length);
        if (result != length) {
            if (report != null) {
                report.append("\n[USB RTL] ").append(label)
                        .append(" failed result ").append(result);
            }
            return -1;
        }
        if (length == 1) {
            return data[0] & 0xff;
        }
        return ((data[1] & 0xff) << 8) | (data[0] & 0xff);
    }

    private boolean rtlWriteArray(int block, int address, byte[] data, int length,
                                  String label, StringBuilder report) {
        int requestIndex = ((block & 0xff) << 8) | 0x10;
        int result = rtlControlOut(address & 0xffff, requestIndex, data, length);
        if (result != length) {
            report.append("\n[USB RTL] ").append(label)
                    .append(" write failed result ").append(result);
            return false;
        }
        return true;
    }

    private int rtlReadArray(int block, int address, byte[] data, int length,
                             String label, StringBuilder report) {
        int requestIndex = (block & 0xff) << 8;
        int result = rtlControlIn(address & 0xffff, requestIndex, data, length);
        if (result != length && report != null) {
            report.append("\n[USB RTL] ").append(label)
                    .append(" read failed result ").append(result);
        }
        return result;
    }

    private int rtlControlOut(int value, int index, byte[] data, int length) {
        if (activeConnection == null) {
            return -1;
        }
        return activeConnection.controlTransfer(USB_VENDOR_OUT, 0, value, index,
                data, length, CTRL_TIMEOUT_MS);
    }

    private int rtlControlIn(int value, int index, byte[] data, int length) {
        if (activeConnection == null) {
            return -1;
        }
        return activeConnection.controlTransfer(USB_VENDOR_IN, 0, value, index,
                data, length, CTRL_TIMEOUT_MS);
    }

    String runNativeBulkBenchmark() {
        StringBuilder report = new StringBuilder();
        stopPreviewThread(1200L);
        if (activeConnection == null || activeBulkInEndpoint == null) {
            String openReport = openReceiverSession();
            report.append(openReport);
            if (activeConnection == null || activeBulkInEndpoint == null) {
                return report.toString();
            }
            report.append('\n');
        }
        if (activeReceiverKind == ActiveUsbReceiverKind.RTL_SDR) {
            return runRtlSdrBulkProbe(report);
        }
        if (!NativeUsbBridge.isAvailable()) {
            return report.append("[USB OTG] ")
                    .append(NativeUsbBridge.bulkBenchmark(-1, 0, 0, 0, 1))
                    .toString();
        }

        boolean openedByCommand = false;
        boolean started = false;
        try {
            int result = fx3Command(CMD_OPEN, 0);
            openedByCommand = result >= 0;
            report.append("[USB OTG] native bench CMD_OPEN result ").append(result);

            result = controlOutU64(CMD_SET_FREQ, 100_000_000L);
            report.append("\n[USB OTG] native bench SET_FREQ 100 MHz result ").append(result);

            result = controlOutU64(CMD_SET_SR, 8_000_000L);
            report.append("\n[USB OTG] native bench SET_SR 8 Msps result ").append(result);

            result = controlOutU64(CMD_SET_AUTOBW, Math.round(0.9 * 1024.0));
            report.append("\n[USB OTG] native bench SET_AUTOBW 0.9 result ").append(result);

            int packsPerTransfer = 16;
            result = fx3Command(CMD_START, packsPerTransfer);
            started = result >= 0;
            report.append("\n[USB OTG] native bench CMD_START packs ")
                    .append(packsPerTransfer)
                    .append(" result ")
                    .append(result);
            if (!started) {
                return report.toString();
            }

            int fd = activeConnection.getFileDescriptor();
            int endpoint = activeBulkInEndpoint.getAddress();
            int transferBytes = previewTransferBytes(packsPerTransfer);
            report.append("\n[USB OTG] native bench fd ")
                    .append(fd)
                    .append(" ep 0x")
                    .append(Integer.toHexString(endpoint & 0xff))
                    .append(" transfer ")
                    .append(transferBytes);
            int[][] matrix = {
                    {4_096, 0},
                    {16_384, 0},
                    {65_536, 0},
                    {4_096, 2},
                    {16_384, 2},
                    {16_384, 4},
                    {32_768, 2},
                    {32_768, 4},
                    {65_536, 1},
                    {65_536, 2},
                    {65_536, 4}
            };
            for (int[] item : matrix) {
                String nativeResult = NativeUsbBridge.bulkBenchmark(fd,
                        endpoint,
                        item[0],
                        1200,
                        item[1]);
                report.append("\n[USB OTG] ").append(nativeResult);
            }
        } catch (RuntimeException e) {
            report.append("\n[USB OTG] native bench failed: ").append(e.getMessage());
        } finally {
            if (started) {
                int stopResult = fx3Command(CMD_STOP, 0);
                report.append("\n[USB OTG] native bench CMD_STOP result ").append(stopResult);
            }
            if (openedByCommand) {
                int closeResult = fx3Command(CMD_CLOSE, 0);
                report.append("\n[USB OTG] native bench CMD_CLOSE result ").append(closeResult);
            }
        }
        return report.toString();
    }

    private void runPreviewLoop(PreviewRequest request) {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (RuntimeException ignored) {
        }

        boolean openedByCommand = false;
        boolean started = false;
        boolean loggedFirstFrame = false;
        long lastFrameNanos = 0L;
        long telemetryStartNanos = System.nanoTime();
        long telemetryBytes = 0L;
        long telemetryReads = 0L;
        long telemetryIqSamples = 0L;
        long telemetryPcmSamples = 0L;
        long telemetryAudioFrames = 0L;
        long telemetryFftFrames = 0L;
        int readFailures = 0;
        int packsPerTransfer = activeAgileApi ? 16 : 1;
        int usbReadBytes = previewReadTransferBytes(packsPerTransfer);
        byte[] buffer = new byte[usbReadBytes];
        int[] nativeAudioStats = new int[3];
        List<QueuedUsbRead> queuedReads = new ArrayList<>();
        boolean queuedIo = activeAgileApi;
        try {
            int result = fx3Command(CMD_OPEN, 0);
            openedByCommand = result >= 0;
            log("[USB OTG] preview CMD_OPEN result " + result);

            result = configureAndStartPreviewHardware(request.centerFrequency,
                    request.sampleRate,
                    request.inputMode,
                    request.lnaGain,
                    request.vgaGain,
                    packsPerTransfer,
                    "");
            if (result < 0) {
                log("[USB OTG] preview CMD_START failed; retrying clean open/start once");
                fx3Command(CMD_STOP, 0);
                fx3Command(CMD_CLOSE, 0);
                openedByCommand = false;
                sleepQuietly(250L);
                result = fx3Command(CMD_OPEN, 0);
                openedByCommand = result >= 0;
                log("[USB OTG] preview retry CMD_OPEN result " + result);
                if (openedByCommand) {
                    result = configureAndStartPreviewHardware(request.centerFrequency,
                            request.sampleRate,
                            request.inputMode,
                            request.lnaGain,
                            request.vgaGain,
                            packsPerTransfer,
                            " retry");
                }
            }
            started = result >= 0;
            if (!started) {
                log("[USB OTG] preview start failed; stream is not running");
                return;
            }
            if (queuedIo) {
                queuedIo = startPreviewQueuedReads(queuedReads, buffer.length);
                log("[USB OTG] preview queued USB IO " + (queuedIo ? "enabled" : "unavailable")
                        + " requests " + queuedReads.size()
                        + " bytes " + buffer.length);
            }

            while (previewRunning) {
                byte[] readBuffer = buffer;
                QueuedUsbRead queuedRead = null;
                if (queuedIo) {
                    UsbRequest completed;
                    try {
                        completed = activeConnection.requestWait(250);
                    } catch (TimeoutException e) {
                        completed = null;
                    }
                    if (completed == null) {
                        ++readFailures;
                        if (readFailures >= 8) {
                            log("[USB OTG] preview queued read stalled");
                            break;
                        }
                        continue;
                    }
                    Object clientData = completed.getClientData();
                    if (!(clientData instanceof QueuedUsbRead)) {
                        ++readFailures;
                        if (readFailures >= 8) {
                            log("[USB OTG] preview queued read returned unknown request");
                            break;
                        }
                        continue;
                    }
                    queuedRead = (QueuedUsbRead) clientData;
                    result = queuedRead.copyCompletedBytes();
                    readBuffer = queuedRead.bytes;
                    if (!queuePreviewUsbRead(queuedRead, buffer.length)) {
                        log("[USB OTG] preview queued read immediate requeue failed");
                        break;
                    }
                    queuedRead = null;
                } else {
                    result = activeConnection.bulkTransfer(activeBulkInEndpoint, buffer, buffer.length, 250);
                }
                if (result <= 0) {
                    ++readFailures;
                    if (queuedRead != null && !queuePreviewUsbRead(queuedRead, buffer.length)) {
                        log("[USB OTG] preview queued read requeue failed after empty result");
                        break;
                    }
                    if (readFailures >= 8) {
                        log("[USB OTG] preview bulk read stalled result " + result);
                        break;
                    }
                    continue;
                }
                readFailures = 0;
                telemetryBytes += result;
                ++telemetryReads;
                int complexSamples = rawFobosComplexSampleCount(readBuffer, result);
                if (complexSamples < 2) {
                    if (queuedRead != null && !queuePreviewUsbRead(queuedRead, buffer.length)) {
                        log("[USB OTG] preview queued read requeue failed after short block");
                        break;
                    }
                    continue;
                }
                telemetryIqSamples += complexSamples;
                long now = System.nanoTime();
                double audioSampleRate = updatePreviewAudioSampleRate(now,
                        complexSamples,
                        previewSampleRate);
                float[] iqSamples = null;
                if (previewAudioEnabled && listener != null) {
                    if (previewAudioResetRequested) {
                        resetAudioDemodState();
                        NativeUsbBridge.resetAudioDemodState();
                        previewAudioResetRequested = false;
                    }
                    byte[] pcm;
                    if (NativeUsbBridge.isAvailable()) {
                        nativeAudioStats[0] = 0;
                        nativeAudioStats[1] = 0;
                        nativeAudioStats[2] = 0;
                        pcm = NativeUsbBridge.audioFromRawFobosIq(readBuffer,
                                result,
                                previewCenterFrequency,
                                previewListeningFrequency,
                                previewSampleRate,
                                audioSampleRate,
                                previewInputMode,
                                previewModulationType,
                                previewBandwidth,
                                activeAgileApi,
                                nativeAudioStats);
                        previewLastPcmPeak = nativeAudioStats[0];
                        previewLastAudioOutputSamples = nativeAudioStats[1];
                        previewLastAudioOffsetHz = nativeAudioStats[2];
                    } else {
                        pcm = audioFrameFromRawFobosIq(readBuffer, result, previewCenterFrequency,
                                previewListeningFrequency, previewSampleRate, audioSampleRate,
                                previewInputMode, previewModulationType);
                    }
                    if (pcm.length > 0) {
                        telemetryPcmSamples += pcm.length / 2;
                        ++telemetryAudioFrames;
                        listener.onUsbAudio(pcm);
                    }
                }
                if (now - telemetryStartNanos >= TELEMETRY_INTERVAL_NANOS) {
                    emitPreviewTelemetry(now,
                            telemetryStartNanos,
                            telemetryBytes,
                            telemetryReads,
                            telemetryIqSamples,
                            telemetryPcmSamples,
                            telemetryAudioFrames,
                            telemetryFftFrames);
                    telemetryStartNanos = now;
                    telemetryBytes = 0L;
                    telemetryReads = 0L;
                    telemetryIqSamples = 0L;
                    telemetryPcmSamples = 0L;
                    telemetryAudioFrames = 0L;
                    telemetryFftFrames = 0L;
                }
                long frameIntervalNanos = previewAudioEnabled ? 450_000_000L : 90_000_000L;
                if (now - lastFrameNanos < frameIntervalNanos) {
                    if (queuedRead != null && !queuePreviewUsbRead(queuedRead, buffer.length)) {
                        log("[USB OTG] preview queued read requeue failed after throttled FFT");
                        break;
                    }
                    continue;
                }
                lastFrameNanos = now;
                if (iqSamples == null) {
                    iqSamples = convertRawFobosIq(readBuffer, result, previewInputMode);
                }
                FobosNetworkClient.SpectrumFrame frame =
                        spectrumFrameFromIq(iqSamples, previewCenterFrequency,
                                previewListeningFrequency, previewSampleRate,
                                previewInputMode,
                                previewFftLength,
                                previewBandwidth,
                                previewModulationType,
                                previewRequestedFftLength);
                if (!loggedFirstFrame && frame != null) {
                    loggedFirstFrame = true;
                    log("[USB OTG] preview first FFT frame " + frame.magnitudes.length + " bins");
                }
                if (listener != null && frame != null) {
                    ++telemetryFftFrames;
                    listener.onUsbSpectrum(frame);
                }
                if (queuedRead != null && !queuePreviewUsbRead(queuedRead, buffer.length)) {
                    log("[USB OTG] preview queued read requeue failed after FFT");
                    break;
                }
            }
        } catch (RuntimeException e) {
            log("[USB OTG] preview failed: " + e.getMessage());
        } finally {
            closePreviewQueuedReads(queuedReads);
            if (started) {
                int stopResult = fx3Command(CMD_STOP, 0);
                log("[USB OTG] preview CMD_STOP result " + stopResult);
            }
            if (openedByCommand) {
                int closeResult = fx3Command(CMD_CLOSE, 0);
                log("[USB OTG] preview CMD_CLOSE result " + closeResult);
            }
            previewRunning = false;
            previewThread = null;
            log("[USB OTG] preview stopped");
        }
    }

    private void runRtlPreviewLoop(PreviewRequest request) {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (RuntimeException ignored) {
        }

        ArrayBlockingQueue<byte[]> bufferPool = new ArrayBlockingQueue<>(RTL_PREVIEW_BUFFER_POOL_DEPTH);
        for (int i = 0; i < RTL_PREVIEW_BUFFER_POOL_DEPTH; ++i) {
            bufferPool.offer(new byte[RTL_PREVIEW_TRANSFER_BYTES]);
        }
        ArrayBlockingQueue<RtlIqBlock> audioQueue = new ArrayBlockingQueue<>(RTL_AUDIO_QUEUE_DEPTH);
        ArrayBlockingQueue<RtlIqBlock> spectrumQueue = new ArrayBlockingQueue<>(RTL_SPECTRUM_QUEUE_DEPTH);
        AtomicLong telemetryBytes = new AtomicLong();
        AtomicLong telemetryReads = new AtomicLong();
        AtomicLong telemetryIqSamples = new AtomicLong();
        AtomicLong telemetryPcmSamples = new AtomicLong();
        AtomicLong telemetryAudioFrames = new AtomicLong();
        AtomicLong telemetryFftFrames = new AtomicLong();
        AtomicLong telemetryAudioDrops = new AtomicLong();
        AtomicLong telemetrySpectrumDrops = new AtomicLong();
        Thread audioThread = new Thread(
                () -> runRtlAudioWorker(audioQueue, bufferPool, telemetryPcmSamples,
                        telemetryAudioFrames),
                "RtlOtgAudio");
        Thread spectrumThread = new Thread(
                () -> runRtlSpectrumWorker(spectrumQueue, bufferPool, telemetryFftFrames),
                "RtlOtgSpectrum");
        long lastSpectrumOfferNanos = 0L;
        long telemetryStartNanos = System.nanoTime();
        int readFailures = 0;

        try {
            StringBuilder initReport = new StringBuilder();
            int centerHz = rtlPreviewFrequencyHz(request.centerFrequency);
            int sampleRateHz = (int) Math.round(rtlPreviewSampleRate(request.sampleRate));
            if (!rtlEnsureInitialized(initReport, centerHz, sampleRateHz)) {
                log(initReport.append("\n[USB RTL] preview init failed").toString());
                return;
            }
            if (rtlTunerI2cAddress != 0) {
                rtlSetI2cRepeater(true, initReport);
                rtlR82xxSetGain(rtlGainTenthsFromUi(request.lnaGain, request.vgaGain), initReport);
                rtlSetI2cRepeater(false, initReport);
            }
            audioThread.start();
            spectrumThread.start();
            log(initReport.append("\n[USB RTL] preview init complete")
                    .append("\n[USB RTL] pipeline threads: reader/audio/spectrum")
                    .toString());

            while (previewRunning) {
                byte[] buffer = acquireRtlBuffer(bufferPool);
                int result;
                try {
                    result = activeConnection.bulkTransfer(activeBulkInEndpoint, buffer, buffer.length, 250);
                } catch (RuntimeException e) {
                    recycleRtlBuffer(bufferPool, buffer);
                    if (previewRunning && System.nanoTime() < rtlReadTransientUntilNanos) {
                        readFailures = 0;
                        continue;
                    }
                    log("[USB RTL] preview bulk read failed: " + e.getMessage());
                    break;
                }
                if (result <= 0) {
                    recycleRtlBuffer(bufferPool, buffer);
                    if (System.nanoTime() < rtlReadTransientUntilNanos) {
                        readFailures = 0;
                        continue;
                    }
                    ++readFailures;
                    if (readFailures >= 8) {
                        log("[USB RTL] preview bulk read stalled result " + result);
                        break;
                    }
                    continue;
                }
                readFailures = 0;
                telemetryBytes.addAndGet(result);
                telemetryReads.incrementAndGet();
                int complexSamples = rawRtlComplexSampleCount(buffer, result);
                if (complexSamples < 2) {
                    recycleRtlBuffer(bufferPool, buffer);
                    continue;
                }
                telemetryIqSamples.addAndGet(complexSamples);

                long now = System.nanoTime();
                updatePreviewAudioSampleRate(now,
                        complexSamples,
                        previewSampleRate,
                        false,
                        0.20);
                RtlIqBlock block = new RtlIqBlock(buffer,
                        result,
                        now,
                        complexSamples,
                        previewCenterFrequency,
                        previewListeningFrequency,
                        previewSampleRate,
                        previewBandwidth,
                        previewModulationType,
                        previewFftLength,
                        previewRequestedFftLength);
                boolean queued = false;
                boolean attemptedQueue = false;
                if (previewAudioEnabled && listener != null) {
                    attemptedQueue = true;
                    queued |= offerRtlBlock(audioQueue, block, bufferPool, telemetryAudioDrops);
                }
                long frameIntervalNanos = previewAudioEnabled ? 260_000_000L : 80_000_000L;
                if (listener != null && now - lastSpectrumOfferNanos >= frameIntervalNanos) {
                    lastSpectrumOfferNanos = now;
                    attemptedQueue = true;
                    queued |= offerRtlBlock(spectrumQueue, block, bufferPool, telemetrySpectrumDrops);
                }
                if (!queued && (!attemptedQueue || block.references.get() <= 0)) {
                    recycleRtlBuffer(bufferPool, buffer);
                }
                if (now - telemetryStartNanos >= TELEMETRY_INTERVAL_NANOS) {
                    emitPreviewTelemetry(now,
                            telemetryStartNanos,
                            telemetryBytes.getAndSet(0L),
                            telemetryReads.getAndSet(0L),
                            telemetryIqSamples.getAndSet(0L),
                            telemetryPcmSamples.getAndSet(0L),
                            telemetryAudioFrames.getAndSet(0L),
                            telemetryFftFrames.getAndSet(0L));
                    long audioDrops = telemetryAudioDrops.getAndSet(0L);
                    long spectrumDrops = telemetrySpectrumDrops.getAndSet(0L);
                    if (audioDrops > 0L || spectrumDrops > 0L) {
                        log("[USB RTL] pipeline drops audio " + audioDrops +
                                ", spectrum " + spectrumDrops);
                    }
                    telemetryStartNanos = now;
                }
            }
        } catch (RuntimeException e) {
            log("[USB RTL] preview failed: " + e.getMessage());
        } finally {
            previewRunning = false;
            joinThreadQuietly(audioThread, 600L);
            joinThreadQuietly(spectrumThread, 600L);
            drainRtlQueue(audioQueue, bufferPool);
            drainRtlQueue(spectrumQueue, bufferPool);
            previewThread = null;
            log("[USB RTL] preview stopped");
        }
    }

    private byte[] acquireRtlBuffer(ArrayBlockingQueue<byte[]> bufferPool) {
        byte[] buffer = bufferPool.poll();
        return buffer != null ? buffer : new byte[RTL_PREVIEW_TRANSFER_BYTES];
    }

    private void recycleRtlBuffer(ArrayBlockingQueue<byte[]> bufferPool, byte[] buffer) {
        if (buffer != null && buffer.length == RTL_PREVIEW_TRANSFER_BYTES) {
            bufferPool.offer(buffer);
        }
    }

    private boolean offerRtlBlock(ArrayBlockingQueue<RtlIqBlock> queue,
                                  RtlIqBlock block,
                                  ArrayBlockingQueue<byte[]> bufferPool,
                                  AtomicLong drops) {
        block.references.incrementAndGet();
        if (queue.offer(block)) {
            return true;
        }
        RtlIqBlock old = queue.poll();
        if (old != null) {
            releaseRtlBlock(old, bufferPool);
            if (drops != null) {
                drops.incrementAndGet();
            }
        }
        if (queue.offer(block)) {
            return true;
        }
        block.references.decrementAndGet();
        if (drops != null) {
            drops.incrementAndGet();
        }
        return false;
    }

    private void releaseRtlBlock(RtlIqBlock block, ArrayBlockingQueue<byte[]> bufferPool) {
        if (block == null) {
            return;
        }
        if (block.references.decrementAndGet() <= 0) {
            recycleRtlBuffer(bufferPool, block.bytes);
        }
    }

    private void drainRtlQueue(ArrayBlockingQueue<RtlIqBlock> queue,
                               ArrayBlockingQueue<byte[]> bufferPool) {
        RtlIqBlock block;
        while ((block = queue.poll()) != null) {
            releaseRtlBlock(block, bufferPool);
        }
    }

    private void joinThreadQuietly(Thread thread, long timeoutMs) {
        if (thread == null || thread == Thread.currentThread()) {
            return;
        }
        try {
            thread.join(Math.max(0L, timeoutMs));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void runRtlAudioWorker(ArrayBlockingQueue<RtlIqBlock> audioQueue,
                                   ArrayBlockingQueue<byte[]> bufferPool,
                                   AtomicLong telemetryPcmSamples,
                                   AtomicLong telemetryAudioFrames) {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (RuntimeException ignored) {
        }
        while (previewRunning || !audioQueue.isEmpty()) {
            RtlIqBlock block = null;
            try {
                block = audioQueue.poll(80L, TimeUnit.MILLISECONDS);
                if (block == null) {
                    continue;
                }
                if (!previewAudioEnabled || listener == null) {
                    continue;
                }
                if (previewAudioResetRequested) {
                    resetAudioDemodState();
                    NativeUsbBridge.resetAudioDemodState();
                    previewAudioResetRequested = false;
                }
                double timingSampleRate = previewAudioSampleRate > 0.0
                        ? previewAudioSampleRate
                        : previewSampleRate;
                byte[] pcm = audioFrameFromRawRtlIq(block.bytes,
                        block.length,
                        block.centerFrequency,
                        block.listeningFrequency,
                        block.sampleRate,
                        timingSampleRate,
                        block.bandwidth,
                        block.modulationType);
                if (pcm.length > 0) {
                    telemetryPcmSamples.addAndGet(pcm.length / 2);
                    telemetryAudioFrames.incrementAndGet();
                    listener.onUsbAudio(pcm);
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            } catch (RuntimeException e) {
                log("[USB RTL] audio worker failed: " + e.getMessage());
            } finally {
                if (block != null) {
                    releaseRtlBlock(block, bufferPool);
                }
            }
        }
    }

    private void runRtlSpectrumWorker(ArrayBlockingQueue<RtlIqBlock> spectrumQueue,
                                      ArrayBlockingQueue<byte[]> bufferPool,
                                      AtomicLong telemetryFftFrames) {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DISPLAY);
        } catch (RuntimeException ignored) {
        }
        boolean loggedFirstFrame = false;
        while (previewRunning || !spectrumQueue.isEmpty()) {
            RtlIqBlock block = null;
            try {
                block = spectrumQueue.poll(120L, TimeUnit.MILLISECONDS);
                if (block == null) {
                    continue;
                }
                if (listener == null) {
                    continue;
                }
                float[] iqSamples = convertRawRtlIq(block.bytes, block.length);
                FobosNetworkClient.SpectrumFrame frame =
                        spectrumFrameFromIq(iqSamples,
                                block.centerFrequency,
                                block.listeningFrequency,
                                block.sampleRate,
                                RadioSettings.INPUT_RF,
                                block.fftLength,
                                block.bandwidth,
                                block.modulationType,
                                block.requestedFftLength);
                if (!loggedFirstFrame && frame != null) {
                    loggedFirstFrame = true;
                    log("[USB RTL] preview first FFT frame " + frame.magnitudes.length + " bins");
                }
                if (frame != null) {
                    telemetryFftFrames.incrementAndGet();
                    listener.onUsbSpectrum(frame);
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            } catch (RuntimeException e) {
                log("[USB RTL] spectrum worker failed: " + e.getMessage());
            } finally {
                if (block != null) {
                    releaseRtlBlock(block, bufferPool);
                }
            }
        }
    }

    private void startPreviewRestartWorkerLocked() {
        previewRestartWorkerRunning = true;
        Thread worker = new Thread(this::previewRestartWorkerLoop, "FobosOtgSettings");
        worker.setDaemon(true);
        worker.start();
    }

    private void previewRestartWorkerLoop() {
        while (true) {
            PreviewRequest request;
            synchronized (previewControlLock) {
                if (previewRestartCancelled) {
                    pendingPreviewRestart = null;
                    previewRestartWorkerRunning = false;
                    return;
                }
                request = pendingPreviewRestart;
                pendingPreviewRestart = null;
                if (request == null) {
                    previewRestartWorkerRunning = false;
                    return;
                }
            }

            stopPreviewThread(1000L);

            synchronized (previewControlLock) {
                if (previewRestartCancelled) {
                    previewRestartWorkerRunning = false;
                    return;
                }
                if (previewThread != null) {
                    pendingPreviewRestart = request;
                    log("[USB OTG] queued preview restart is waiting for the old preview thread to stop");
                }
            }
            if (previewThread != null) {
                sleepQuietly(250L);
                continue;
            }

            synchronized (previewControlLock) {
                if (previewRestartCancelled) {
                    previewRestartWorkerRunning = false;
                    return;
                }
            }

            String message = startReceiverPreviewNow(request);
            log("[USB OTG] queued preview restart applied\n" + message);
        }
    }

    private void emitPreviewTelemetry(long nowNanos,
                                      long startNanos,
                                      long bytesRead,
                                      long reads,
                                      long iqSamples,
                                      long pcmSamples,
                                      long audioFrames,
                                      long fftFrames) {
        if (listener == null) {
            return;
        }
        double elapsedSeconds = Math.max(0.001, (nowNanos - startNanos) / 1_000_000_000.0);
        double usbMbps = (bytesRead * 8.0) / (elapsedSeconds * 1_000_000.0);
        double iqMsps = iqSamples / (elapsedSeconds * 1_000_000.0);
        double pcmRate = pcmSamples / elapsedSeconds;
        double readRate = reads / elapsedSeconds;
        double fftRate = fftFrames / elapsedSeconds;
        double audioMsps = previewAudioSampleRate / 1_000_000.0;
        String message = String.format(Locale.US,
                "USB %.1f Mb/s, %.2f MS/s, %.0f reads/s, PCM %.0f/s, audio %d/s, FFT %.1f/s, audioSR %.2f MS/s, peak %d, out %d, off %.0f Hz",
                usbMbps,
                iqMsps,
                readRate,
                pcmRate,
                Math.round(audioFrames / elapsedSeconds),
                fftRate,
                audioMsps,
                previewLastPcmPeak,
                previewLastAudioOutputSamples,
                previewLastAudioOffsetHz);
        listener.onUsbTelemetry(message);
    }

    private int configureAndStartPreviewHardware(double centerFrequency,
                                                 double sampleRate,
                                                 int inputMode,
                                                 int lnaGain,
                                                 int vgaGain,
                                                 int packsPerTransfer,
                                                 String logSuffix) {
        int directMode = inputMode == RadioSettings.INPUT_RF ? 0 : 1;
        int result = fx3Command(CMD_SET_DIRECT, directMode);
        log("[USB OTG] preview" + logSuffix + " SET_DIRECT " + directMode + " result " + result);

        if (inputMode == RadioSettings.INPUT_RF) {
            result = controlOutU64(CMD_SET_FREQ, Math.max(50_000_000L, Math.round(centerFrequency)));
            log("[USB OTG] preview" + logSuffix + " SET_FREQ result " + result);
        }

        result = controlOutU64(CMD_SET_SR, Math.round(sampleRate));
        log("[USB OTG] preview" + logSuffix + " SET_SR result " + result);

        result = controlOutU64(CMD_SET_AUTOBW, Math.round(0.9 * 1024.0));
        log("[USB OTG] preview" + logSuffix + " SET_AUTOBW result " + result);

        result = fx3Command(CMD_SET_LNA, Math.max(0, Math.min(3, lnaGain)));
        log("[USB OTG] preview" + logSuffix + " SET_LNA result " + result);

        result = fx3Command(CMD_SET_VGA, Math.max(0, Math.min(31, vgaGain)));
        log("[USB OTG] preview" + logSuffix + " SET_VGA result " + result);

        int startValue = activeAgileApi ? packsPerTransfer : 1;
        result = fx3Command(CMD_START, startValue);
        log("[USB OTG] preview" + logSuffix + " CMD_START result " + result);
        return result;
    }

    private int previewTransferBytes(int packsPerTransfer) {
        int normalizedPacks = Math.max(1, packsPerTransfer);
        if (activeAgileApi) {
            return 4096 * normalizedPacks * 4;
        }
        return 256 * 1024;
    }

    private int previewReadTransferBytes(int packsPerTransfer) {
        if (activeAgileApi) {
            return 64 * 1024;
        }
        return previewTransferBytes(packsPerTransfer);
    }

    private boolean startPreviewQueuedReads(List<QueuedUsbRead> queuedReads, int transferBytes) {
        queuedReads.clear();
        if (activeConnection == null || activeBulkInEndpoint == null || transferBytes <= 0) {
            return false;
        }
        final int requestCount = activeAgileApi ? 6 : 4;
        for (int i = 0; i < requestCount; ++i) {
            UsbRequest request = new UsbRequest();
            if (!request.initialize(activeConnection, activeBulkInEndpoint)) {
                closePreviewQueuedReads(queuedReads);
                request.close();
                return false;
            }
            queuedReads.add(new QueuedUsbRead(request, transferBytes));
        }
        for (QueuedUsbRead queuedRead : queuedReads) {
            if (!queuePreviewUsbRead(queuedRead, transferBytes)) {
                closePreviewQueuedReads(queuedReads);
                return false;
            }
        }
        return true;
    }

    @SuppressWarnings("deprecation")
    private boolean queuePreviewUsbRead(QueuedUsbRead queuedRead, int transferBytes) {
        if (queuedRead == null || queuedRead.request == null) {
            return false;
        }
        queuedRead.buffer.clear();
        queuedRead.buffer.limit(Math.min(transferBytes, queuedRead.bytes.length));
        return queuedRead.request.queue(queuedRead.buffer, queuedRead.buffer.limit());
    }

    private void closePreviewQueuedReads(List<QueuedUsbRead> queuedReads) {
        if (queuedReads == null) {
            return;
        }
        for (QueuedUsbRead queuedRead : queuedReads) {
            if (queuedRead == null || queuedRead.request == null) {
                continue;
            }
            try {
                queuedRead.request.cancel();
            } catch (RuntimeException ignored) {
            }
            queuedRead.request.close();
        }
        queuedReads.clear();
    }

    private void sleepQuietly(long millis) {
        try {
            Thread.sleep(Math.max(0L, millis));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void handleUsbBroadcast(Intent intent) {
        if (intent == null) {
            return;
        }
        String action = intent.getAction();
        UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        if (ACTION_USB_PERMISSION.equals(action)) {
            boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
            if (device == null) {
                log("[USB] permission result without device");
                return;
            }
            if (!granted) {
                openSessionAfterPermission = false;
                log("[USB] permission denied for " + shortDeviceName(device));
                return;
            }
            if (openSessionAfterPermission) {
                openSessionAfterPermission = false;
                log(openReceiverSession(device));
                return;
            }
            log(probeOpenDevice(device));
            return;
        }
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
            log("[USB] attached: " + (device != null ? shortDeviceName(device) : "unknown device"));
            log(scanReport());
            return;
        }
        if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
            log("[USB] detached: " + (device != null ? shortDeviceName(device) : "unknown device"));
            if (device != null && activeDevice != null &&
                    device.getDeviceName().equals(activeDevice.getDeviceName())) {
                closeActiveSession();
                log("[USB OTG] active session closed after detach");
            }
        }
    }

    private UsbDevice bestDevice() {
        List<UsbDevice> devices = sortedDevices();
        return devices.isEmpty() ? null : devices.get(0);
    }

    private String noUsbDevicesMessage(String prefix) {
        return prefix + " no devices visible to Android USB host API" +
                "\n" + prefix + " disconnect the phone from the PC USB cable and use a powered OTG hub for the receiver";
    }

    private List<UsbDevice> sortedDevices() {
        HashMap<String, UsbDevice> deviceList = usbManager.getDeviceList();
        List<UsbDevice> devices = new ArrayList<>(deviceList.values());
        devices.sort(Comparator
                .comparingInt(this::devicePriority)
                .thenComparing(UsbDevice::getDeviceName));
        return devices;
    }

    private void appendDeviceReport(StringBuilder report, UsbDevice device) {
        report.append('\n')
                .append(isKnownReceiverCandidate(device) ? "  * " : "  - ")
                .append(shortDeviceName(device))
                .append(" [").append(receiverKindLabel(device)).append(']')
                .append('\n')
                .append("    class ").append(formatByte(device.getDeviceClass()))
                .append(" subclass ").append(formatByte(device.getDeviceSubclass()))
                .append(" protocol ").append(formatByte(device.getDeviceProtocol()))
                .append('\n')
                .append("    manufacturer: ").append(safeManufacturer(device))
                .append(" product: ").append(safeProduct(device))
                .append(" serial: ").append(safeSerial(device))
                .append('\n')
                .append("    permission: ").append(usbManager.hasPermission(device) ? "granted" : "not granted")
                .append(" interfaces: ").append(device.getInterfaceCount());
        for (int i = 0; i < device.getInterfaceCount(); ++i) {
            UsbInterface usbInterface = device.getInterface(i);
            report.append('\n')
                    .append("      if").append(i)
                    .append(" id ").append(usbInterface.getId())
                    .append(" class ").append(formatByte(usbInterface.getInterfaceClass()))
                    .append(" subclass ").append(formatByte(usbInterface.getInterfaceSubclass()))
                    .append(" protocol ").append(formatByte(usbInterface.getInterfaceProtocol()))
                    .append(" endpoints ").append(usbInterface.getEndpointCount());
            for (int j = 0; j < usbInterface.getEndpointCount(); ++j) {
                UsbEndpoint endpoint = usbInterface.getEndpoint(j);
                report.append('\n')
                        .append("        ep").append(j)
                        .append(' ').append(endpointDirection(endpoint))
                        .append(' ').append(endpointType(endpoint))
                        .append(" addr ").append(formatByte(endpoint.getAddress()))
                        .append(" maxPacket ").append(endpoint.getMaxPacketSize())
                        .append(" interval ").append(endpoint.getInterval());
            }
        }
    }

    private String probeOpenDevice(UsbDevice device) {
        StringBuilder report = new StringBuilder();
        report.append("[USB] permission granted for ").append(shortDeviceName(device));
        UsbDeviceConnection connection = null;
        try {
            connection = usbManager.openDevice(device);
            if (connection == null) {
                report.append("\n[USB] openDevice returned null");
                return report.toString();
            }
            byte[] rawDescriptors = connection.getRawDescriptors();
            report.append("\n[USB] openDevice OK");
            if (rawDescriptors != null) {
                report.append(", raw descriptors ").append(rawDescriptors.length).append(" bytes");
                int bcdDevice = bcdDeviceFromRawDescriptors(rawDescriptors);
                if (bcdDevice >= 0) {
                    report.append(", bcdDevice ").append(formatWord(bcdDevice));
                }
            }
            if (isFobosCandidate(device)) {
                report.append("\n[USB] Fobos candidate detected: ")
                        .append(fobosApiHint(rawDescriptors));
            } else if (isRtlSdrCandidate(device)) {
                report.append("\n[USB] RTL-SDR candidate detected: native Android driver path is selected");
            }
        } catch (SecurityException e) {
            report.append("\n[USB] open blocked by Android security: ").append(e.getMessage());
        } catch (RuntimeException e) {
            report.append("\n[USB] open failed: ").append(e.getMessage());
        } finally {
            if (connection != null) {
                connection.close();
            }
        }
        return report.toString();
    }

    private String openReceiverSession(UsbDevice device) {
        if (isRtlSdrCandidate(device)) {
            return openRtlSdrSession(device);
        }
        StringBuilder report = new StringBuilder();
        report.append("[USB OTG] opening receiver session for ").append(shortDeviceName(device));
        closeActiveSession();
        UsbDeviceConnection connection = null;
        try {
            connection = usbManager.openDevice(device);
            if (connection == null) {
                report.append("\n[USB OTG] openDevice returned null");
                return report.toString();
            }
            byte[] rawDescriptors = connection.getRawDescriptors();
            if (rawDescriptors != null) {
                report.append("\n[USB OTG] raw descriptors ").append(rawDescriptors.length)
                        .append(" bytes, ").append(fobosApiHint(rawDescriptors));
            }
            activeAgileApi = bcdDeviceFromRawDescriptors(rawDescriptors) == FOBOS_AGILE_BCD_DEVICE;
            StreamInterfaceChoice choice = chooseStreamInterface(device);
            if (choice == null) {
                report.append("\n[USB OTG] no bulk IN endpoint found; cannot prepare streaming path");
                connection.close();
                return report.toString();
            }
            if (!connection.claimInterface(choice.usbInterface, true)) {
                report.append("\n[USB OTG] claimInterface failed for if")
                        .append(choice.interfaceIndex);
                connection.close();
                return report.toString();
            }
            activeDevice = device;
            activeConnection = connection;
            activeInterface = choice.usbInterface;
            activeBulkInEndpoint = choice.bulkInEndpoint;
            activeBulkOutEndpoint = choice.bulkOutEndpoint;
            activeReceiverKind = ActiveUsbReceiverKind.FOBOS;
            convertDcI = 0.0;
            convertDcQ = 0.0;
            connection = null;
            report.append("\n[USB OTG] claimed if").append(choice.interfaceIndex)
                    .append(" id ").append(activeInterface.getId())
                    .append(", bulk IN ").append(endpointSummary(activeBulkInEndpoint));
            if (activeBulkOutEndpoint != null) {
                report.append(", bulk OUT ").append(endpointSummary(activeBulkOutEndpoint));
            } else {
                report.append(", no bulk OUT endpoint");
            }
            appendFirmwareInfoProbe(report);
            report.append("\n[USB OTG] base ready; OTG preview and sample test are available");
        } catch (SecurityException e) {
            report.append("\n[USB OTG] open blocked by Android security: ").append(e.getMessage());
            if (connection != null) {
                connection.close();
            }
        } catch (RuntimeException e) {
            report.append("\n[USB OTG] open failed: ").append(e.getMessage());
            if (connection != null) {
                connection.close();
            }
        }
        return report.toString();
    }

    private String openRtlSdrSession(UsbDevice device) {
        StringBuilder report = new StringBuilder();
        report.append("[USB RTL] opening receiver session for ").append(shortDeviceName(device));
        closeActiveSession();
        UsbDeviceConnection connection = null;
        try {
            connection = usbManager.openDevice(device);
            if (connection == null) {
                report.append("\n[USB RTL] openDevice returned null");
                return report.toString();
            }
            byte[] rawDescriptors = connection.getRawDescriptors();
            if (rawDescriptors != null) {
                report.append("\n[USB RTL] raw descriptors ")
                        .append(rawDescriptors.length)
                        .append(" bytes, bcdDevice ")
                        .append(formatWord(bcdDeviceFromRawDescriptors(rawDescriptors)));
            }
            StreamInterfaceChoice choice = chooseStreamInterface(device);
            if (choice == null) {
                report.append("\n[USB RTL] no bulk IN endpoint found; cannot prepare streaming path");
                connection.close();
                return report.toString();
            }
            if (!connection.claimInterface(choice.usbInterface, true)) {
                report.append("\n[USB RTL] claimInterface failed for if")
                        .append(choice.interfaceIndex);
                connection.close();
                return report.toString();
            }
            activeDevice = device;
            activeConnection = connection;
            activeInterface = choice.usbInterface;
            activeBulkInEndpoint = choice.bulkInEndpoint;
            activeBulkOutEndpoint = choice.bulkOutEndpoint;
            activeReceiverKind = ActiveUsbReceiverKind.RTL_SDR;
            activeAgileApi = false;
            convertDcI = 0.0;
            convertDcQ = 0.0;
            connection = null;
            report.append("\n[USB RTL] claimed if").append(choice.interfaceIndex)
                    .append(" id ").append(activeInterface.getId())
                    .append(", bulk IN ").append(endpointSummary(activeBulkInEndpoint));
            if (activeBulkOutEndpoint != null) {
                report.append(", bulk OUT ").append(endpointSummary(activeBulkOutEndpoint));
            } else {
                report.append(", no bulk OUT endpoint");
            }
            report.append("\n[USB RTL] base ready; full native streaming still needs RTL2832U/tuner init");
        } catch (SecurityException e) {
            report.append("\n[USB RTL] open blocked by Android security: ").append(e.getMessage());
            if (connection != null) {
                connection.close();
            }
        } catch (RuntimeException e) {
            report.append("\n[USB RTL] open failed: ").append(e.getMessage());
            if (connection != null) {
                connection.close();
            }
        }
        return report.toString();
    }

    private StreamInterfaceChoice chooseStreamInterface(UsbDevice device) {
        StreamInterfaceChoice best = null;
        int bestScore = -1;
        for (int i = 0; i < device.getInterfaceCount(); ++i) {
            UsbInterface usbInterface = device.getInterface(i);
            UsbEndpoint bulkIn = null;
            UsbEndpoint bulkOut = null;
            for (int j = 0; j < usbInterface.getEndpointCount(); ++j) {
                UsbEndpoint endpoint = usbInterface.getEndpoint(j);
                if (endpoint.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) {
                    continue;
                }
                if (endpoint.getDirection() == UsbConstants.USB_DIR_IN && bulkIn == null) {
                    bulkIn = endpoint;
                } else if (endpoint.getDirection() == UsbConstants.USB_DIR_OUT && bulkOut == null) {
                    bulkOut = endpoint;
                }
            }
            if (bulkIn == null) {
                continue;
            }
            int score = 10;
            if (bulkOut != null) {
                score += 5;
            }
            if (usbInterface.getInterfaceClass() == UsbConstants.USB_CLASS_VENDOR_SPEC) {
                score += 2;
            }
            score += usbInterface.getEndpointCount();
            if (score > bestScore) {
                best = new StreamInterfaceChoice(i, usbInterface, bulkIn, bulkOut);
                bestScore = score;
            }
        }
        return best;
    }

    private void appendFirmwareInfoProbe(StringBuilder report) {
        if (activeConnection == null) {
            report.append("\n[USB OTG] firmware info unavailable: no open session");
            return;
        }
        ControlStringResult hw = readFobosInfoString(0);
        ControlStringResult fw = readFobosInfoString(1);
        ControlStringResult build = readFobosInfoString(2);
        report.append("\n[USB OTG] ctrl 0xE8 hw ")
                .append(controlResultSummary(hw))
                .append(", fw ")
                .append(controlResultSummary(fw))
                .append(", build ")
                .append(controlResultSummary(build));
    }

    private ControlStringResult readFobosInfoString(int value) {
        byte[] buffer = new byte[32];
        int result;
        try {
            result = activeConnection.controlTransfer(
                    USB_VENDOR_IN,
                    FOBOS_INFO_REQUEST,
                    value,
                    0,
                    buffer,
                    buffer.length,
                    CTRL_TIMEOUT_MS);
        } catch (RuntimeException e) {
            return new ControlStringResult(-9999, e.getMessage());
        }
        if (result <= 0) {
            return new ControlStringResult(result, "");
        }
        return new ControlStringResult(result, decodeAsciiString(buffer, result));
    }

    private int fx3Command(int command, int value) {
        if (activeConnection == null) {
            return -1;
        }
        return activeConnection.controlTransfer(
                USB_VENDOR_OUT,
                FOBOS_SDR_CMD,
                command,
                value,
                new byte[0],
                0,
                CTRL_TIMEOUT_MS);
    }

    private int controlOutU64(int command, long value) {
        if (activeConnection == null) {
            return -1;
        }
        byte[] buffer = new byte[8];
        for (int i = 0; i < buffer.length; ++i) {
            buffer[i] = (byte) ((value >> (8 * i)) & 0xff);
        }
        return activeConnection.controlTransfer(
                USB_VENDOR_OUT,
                FOBOS_SDR_CMD,
                command,
                0,
                buffer,
                buffer.length,
                CTRL_TIMEOUT_MS);
    }

    private String controlResultSummary(ControlStringResult result) {
        if (result.bytesRead > 0) {
            return "'" + result.value + "' (" + result.bytesRead + " bytes)";
        }
        if (result.value != null && !result.value.isEmpty()) {
            return "error " + result.bytesRead + " " + result.value;
        }
        return "result " + result.bytesRead;
    }

    private String decodeAsciiString(byte[] buffer, int length) {
        int count = Math.max(0, Math.min(length, buffer.length));
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < count; ++i) {
            int value = buffer[i] & 0xff;
            if (value == 0) {
                break;
            }
            if (value >= 32 && value <= 126) {
                builder.append((char) value);
            }
        }
        return builder.toString();
    }

    private void stopPreviewThread(long joinTimeoutMs) {
        previewRunning = false;
        Thread thread = previewThread;
        if (thread == null) {
            return;
        }
        if (thread == Thread.currentThread()) {
            return;
        }
        try {
            thread.join(Math.max(0L, joinTimeoutMs));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (!thread.isAlive()) {
            previewThread = null;
        }
    }

    private int previewFftLength(int requested) {
        int capped = Math.max(256, Math.min(PREVIEW_MAX_FFT, requested > 0 ? requested : 2048));
        int fftLength = 1;
        while ((fftLength << 1) > 0 && (fftLength << 1) <= capped) {
            fftLength <<= 1;
        }
        return Math.max(256, fftLength);
    }

    private int normalizeModulationType(int modulationType) {
        switch (modulationType) {
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_SAM:
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
            case RadioSettings.MOD_DSB:
            case RadioSettings.MOD_CW:
            case RadioSettings.MOD_WFM:
            case RadioSettings.MOD_DMR:
            case RadioSettings.MOD_AM:
                return modulationType;
            default:
                return RadioSettings.MOD_AM;
        }
    }

    private void resetAudioDemodState() {
        audioSourceCursor = 0.0;
        audioMixerPhase = 0.0;
        audioFmLastPhase = 0.0;
        audioFmLastI = 0.0;
        audioFmLastQ = 0.0;
        audioFmPhaseValid = false;
        audioEnvelopeAverage = 0.1;
        audioDcAverage = 0.0;
        audioLowPassAverage = 0.0;
        audioChannelSumI = 0.0;
        audioChannelSumQ = 0.0;
        audioChannelDecimationCount = 0;
        audioChannelLowPassI = 0.0;
        audioChannelLowPassQ = 0.0;
        audioResamplePhase = 0.0;
        audioAgcLevel = 0.05;
        audioFirTaps = new double[0];
        audioFirRingI = new double[0];
        audioFirRingQ = new double[0];
        audioFirRingIndex = 0;
        audioFirConfiguredDecimation = 0;
        audioFirConfiguredModulation = -1;
        audioFirConfiguredSampleRate = 0.0;
        audioFirConfiguredBandwidth = 0.0;
        audioFirOutputI = 0.0;
        audioFirOutputQ = 0.0;
        audioDeemphasisAverage = 0.0;
        audioDebugLastLogNanos = 0L;
        audioRawDcI = 8192.0;
        audioRawDcQ = 8192.0;
        audioRawRtlDcI = 127.5;
        audioRawRtlDcQ = 127.5;
        previewAudioSampleRate = 0.0;
        previewMeasuredSampleRate = 0.0;
        previewAudioRateLastNanos = 0L;
        previewAudioRateWindowStartNanos = 0L;
        previewAudioRateWindowSamples = 0L;
        previewAudioRateAcceptedWindows = 0;
        previewAudioRateIgnoredStartupWindows = 0;
        previewLastPcmPeak = 0;
        previewLastAudioOutputSamples = 0;
        previewLastAudioOffsetHz = 0.0;
        previewAudioResetRequested = false;
    }

    private double initialPreviewAudioSampleRate(double requestedSampleRate) {
        return requestedSampleRate > 0.0 ? requestedSampleRate : PREVIEW_MAX_SAMPLE_RATE;
    }

    private void resetFobosConversionState() {
        convertDcI = 0.0;
        convertDcQ = 0.0;
    }

    private float[] convertRawRtlIq(byte[] buffer, int bytesRead) {
        int usableBytes = Math.max(0, Math.min(bytesRead, buffer.length)) & ~0x1;
        if (usableBytes < 2) {
            return new float[0];
        }
        float[] samples = new float[usableBytes];
        double scale = 1.0 / 128.0;
        double dcRate = 0.0008;
        int outputIndex = 0;
        for (int pos = 0; pos + 1 < usableBytes; pos += 2) {
            double real = (buffer[pos] & 0xff) - 127.5;
            double imag = (buffer[pos + 1] & 0xff) - 127.5;
            convertDcI += dcRate * (real - convertDcI);
            convertDcQ += dcRate * (imag - convertDcQ);
            samples[outputIndex++] = (float) ((real - convertDcI) * scale);
            samples[outputIndex++] = (float) ((imag - convertDcQ) * scale);
        }
        return samples;
    }

    private float[] convertRawFobosIq(byte[] buffer, int bytesRead, int inputMode) {
        int usableBytes = Math.max(0, Math.min(bytesRead, buffer.length)) & ~0x3;
        if (usableBytes < 4) {
            return new float[0];
        }
        float[] samples = new float[(usableBytes / 4) * 2];
        boolean directSampling = RadioSettings.isDirectInput(inputMode);
        int p0 = readLe16(buffer, 0);
        int p1 = usableBytes >= 4 ? readLe16(buffer, 2) : 0;
        boolean rawSwapIq = (p0 & 0x8000) != 0 && (p1 & 0x8000) != 0;
        if (activeAgileApi) {
            boolean swappedHalves = (p0 & 0x4000) != 0 && (p1 & 0x4000) != 0;
            int firstOffset = p0 & p1 & 0x4000;
            int secondOffset = firstOffset ^ 0x4000;
            if (swappedHalves && usableBytes >= 0x8000) {
                int pairs = usableBytes / 0x8000;
                for (int pair = 0; pair < pairs; ++pair) {
                    int inputBase = pair * 0x8000;
                    int outputBase = pair * 0x4000;
                    convertFobosBlock(buffer, inputBase + firstOffset, 0x4000,
                            samples, outputBase, directSampling, rawSwapIq);
                    convertFobosBlock(buffer, inputBase + secondOffset, 0x4000,
                            samples, outputBase + 0x2000, directSampling, rawSwapIq);
                }
                int consumedBytes = pairs * 0x8000;
                if (consumedBytes < usableBytes) {
                    convertFobosBlock(buffer, consumedBytes, usableBytes - consumedBytes,
                            samples, (consumedBytes / 4) * 2, directSampling, rawSwapIq);
                }
                return samples;
            }
        }
        convertFobosBlock(buffer, 0, usableBytes, samples, 0, directSampling, rawSwapIq);
        return samples;
    }

    private void convertFobosBlock(byte[] buffer,
                                   int offset,
                                   int length,
                                   float[] output,
                                   int outputOffset,
                                   boolean directSampling,
                                   boolean rawSwapIq) {
        if (offset < 0 || offset >= buffer.length || outputOffset >= output.length) {
            return;
        }
        int usableBytes = Math.max(0, Math.min(length, buffer.length - offset)) & ~0x3;
        boolean swapIq = directSampling || rawSwapIq != true;
        double scale = 1.0 / 32768.0;
        double dcRate = 0.0004;
        int outputIndex = Math.max(0, outputOffset);
        int end = offset + usableBytes;
        for (int pos = offset; pos + 3 < end && outputIndex + 1 < output.length; pos += 4) {
            int first = readLe16(buffer, pos);
            int second = readLe16(buffer, pos + 2);
            double real = (swapIq ? second : first) & 0x3fff;
            double imag = (swapIq ? first : second) & 0x3fff;
            convertDcI += dcRate * (real - convertDcI);
            convertDcQ += dcRate * (imag - convertDcQ);
            output[outputIndex++] = (float) ((real - convertDcI) * scale);
            output[outputIndex++] = (float) ((imag - convertDcQ) * scale);
        }
    }

    private int readLe16(byte[] buffer, int offset) {
        if (offset < 0 || offset + 1 >= buffer.length) {
            return 0;
        }
        return (buffer[offset] & 0xff) | ((buffer[offset + 1] & 0xff) << 8);
    }

    private int rawFobosComplexSampleCount(byte[] buffer, int bytesRead) {
        int usableBytes = Math.max(0, Math.min(bytesRead, buffer.length)) & ~0x3;
        return usableBytes / 4;
    }

    private int rawRtlComplexSampleCount(byte[] buffer, int bytesRead) {
        int usableBytes = Math.max(0, Math.min(bytesRead, buffer.length)) & ~0x1;
        return usableBytes / 2;
    }

    private double updatePreviewAudioSampleRate(long nowNanos,
                                                int complexSamples,
                                                double requestedSampleRate) {
        return updatePreviewAudioSampleRate(nowNanos,
                complexSamples,
                requestedSampleRate,
                true,
                0.025);
    }

    private double updatePreviewAudioSampleRate(long nowNanos,
                                                int complexSamples,
                                                double requestedSampleRate,
                                                boolean ignoreStartupUnderrate,
                                                double maxStepFraction) {
        if (previewAudioSampleRate <= 0.0) {
            double initialRate = initialPreviewAudioSampleRate(requestedSampleRate);
            previewAudioSampleRate = Math.max(AUDIO_SAMPLE_RATE * 2.0, initialRate);
            previewMeasuredSampleRate = previewAudioSampleRate;
        }
        if (requestedSampleRate >= 7_000_000.0) {
            return previewAudioSampleRate;
        }
        if (previewAudioRateWindowStartNanos <= 0L) {
            previewAudioRateWindowStartNanos = nowNanos;
            previewAudioRateWindowSamples = 0L;
        }
        if (complexSamples > 0) {
            previewAudioRateWindowSamples += complexSamples;
        }
        long elapsedNanos = nowNanos - previewAudioRateWindowStartNanos;
        if (elapsedNanos >= 250_000_000L && previewAudioRateWindowSamples > 0L) {
            double elapsedSeconds = elapsedNanos / 1_000_000_000.0;
            double observedRate = previewAudioRateWindowSamples / elapsedSeconds;
            double upperBound = requestedSampleRate > 0.0
                    ? requestedSampleRate * 1.10
                    : PREVIEW_MAX_SAMPLE_RATE;
            if (observedRate > 100_000.0 && observedRate < upperBound) {
                boolean startupUnderrate = ignoreStartupUnderrate &&
                        previewMeasuredSampleRate > 0.0 &&
                        previewAudioRateAcceptedWindows < 3 &&
                        previewAudioRateIgnoredStartupWindows < 3 &&
                        observedRate < previewMeasuredSampleRate * 0.80;
                if (startupUnderrate) {
                    ++previewAudioRateIgnoredStartupWindows;
                    previewAudioRateWindowStartNanos = nowNanos;
                    previewAudioRateWindowSamples = 0L;
                    previewAudioRateLastNanos = nowNanos;
                    return previewAudioSampleRate;
                }
                double clampedRate = requestedSampleRate > 0.0
                        ? Math.min(requestedSampleRate, observedRate)
                        : observedRate;
                if (previewMeasuredSampleRate <= 0.0) {
                    previewMeasuredSampleRate = clampedRate;
                } else {
                    double maxStep = Math.max(3000.0,
                            previewMeasuredSampleRate * Math.max(0.001, maxStepFraction));
                    double delta = clampedRate - previewMeasuredSampleRate;
                    if (delta > maxStep) {
                        delta = maxStep;
                    } else if (delta < -maxStep) {
                        delta = -maxStep;
                    }
                    previewMeasuredSampleRate += delta;
                }
                previewAudioSampleRate = Math.max(AUDIO_SAMPLE_RATE * 2.0,
                        Math.min(requestedSampleRate > 0.0 ? requestedSampleRate : PREVIEW_MAX_SAMPLE_RATE,
                                previewMeasuredSampleRate * AUDIO_RATE_BIAS));
                ++previewAudioRateAcceptedWindows;
            }
            previewAudioRateWindowStartNanos = nowNanos;
            previewAudioRateWindowSamples = 0L;
        }
        previewAudioRateLastNanos = nowNanos;
        return previewAudioSampleRate;
    }

    private byte[] audioFrameFromRawRtlIq(byte[] buffer,
                                          int bytesRead,
                                          double centerFrequency,
                                          double listeningFrequency,
                                          double nominalSampleRate,
                                          double timingSampleRate,
                                          double bandwidth,
                                          int modulationType) {
        int usableBytes = Math.max(0, Math.min(bytesRead, buffer.length)) & ~0x1;
        int complexSamples = usableBytes / 2;
        if (complexSamples <= 2 ||
                nominalSampleRate <= AUDIO_SAMPLE_RATE ||
                timingSampleRate <= AUDIO_SAMPLE_RATE) {
            return new byte[0];
        }

        double processingSampleRate = Math.max(AUDIO_SAMPLE_RATE * 2.0, nominalSampleRate);
        double playbackSampleRate = Math.max(AUDIO_SAMPLE_RATE * 2.0,
                Math.min(processingSampleRate, timingSampleRate));
        double tuneOffsetHz = listeningFrequency - centerFrequency;
        boolean zeroTuneOffset = Math.abs(tuneOffsetHz) < 1.0;
        if (zeroTuneOffset) {
            tuneOffsetHz = 0.0;
        }
        int inputStride = audioInputStrideForOffset(processingSampleRate,
                modulationType,
                bandwidth,
                tuneOffsetHz);
        double effectiveSampleRate = processingSampleRate / inputStride;
        double effectivePlaybackSampleRate = playbackSampleRate / inputStride;
        double phaseStep = -2.0 * Math.PI * tuneOffsetHz / effectiveSampleRate;
        int decimationFactor = audioChannelDecimationFactor(effectiveSampleRate,
                modulationType,
                bandwidth);
        double channelRate = effectiveSampleRate / Math.max(1, decimationFactor);
        double channelCutoff = Math.min(audioChannelCutoffForMode(modulationType, bandwidth),
                channelRate * 0.45);
        double channelAlpha = clamp01(1.0 - Math.exp(-2.0 * Math.PI * channelCutoff / channelRate));
        double demodCutoff = Math.min(audioDemodCutoffForMode(modulationType, bandwidth),
                channelRate * 0.45);
        double demodAlpha = clamp01(1.0 - Math.exp(-2.0 * Math.PI * demodCutoff / channelRate));
        double outputStep = (AUDIO_SAMPLE_RATE * Math.max(1, decimationFactor)) /
                Math.max(AUDIO_SAMPLE_RATE, effectivePlaybackSampleRate);
        int estimatedOutputSamples = Math.max(8,
                (int) Math.ceil((complexSamples / playbackSampleRate) * AUDIO_SAMPLE_RATE) + 16);
        byte[] pcm = new byte[estimatedOutputSamples * 2];
        int output = 0;
        int decimatedSamples = 0;
        double oscillatorI = zeroTuneOffset ? 1.0 : Math.cos(audioMixerPhase);
        double oscillatorQ = zeroTuneOffset ? 0.0 : Math.sin(audioMixerPhase);
        double stepI = Math.cos(phaseStep);
        double stepQ = Math.sin(phaseStep);
        ensureAudioFirConfigured(decimationFactor, effectiveSampleRate, modulationType, bandwidth);
        double rawPeak = 0.0;
        double channelPeak = 0.0;
        int pcmPeak = 0;
        double rawScale = 1.0 / 128.0;
        double dcRate = Math.min(0.02, 0.0008 * Math.max(1, inputStride));

        for (int sample = 0; sample < complexSamples; sample += inputStride) {
            int pos = sample * 2;
            if (pos + 1 >= usableBytes) {
                continue;
            }
            double real = (buffer[pos] & 0xff);
            double imag = (buffer[pos + 1] & 0xff);
            audioRawRtlDcI += dcRate * (real - audioRawRtlDcI);
            audioRawRtlDcQ += dcRate * (imag - audioRawRtlDcQ);
            double iSample = (real - audioRawRtlDcI) * rawScale;
            double qSample = (imag - audioRawRtlDcQ) * rawScale;
            rawPeak = Math.max(rawPeak, Math.max(Math.abs(iSample), Math.abs(qSample)));

            double mixedI;
            double mixedQ;
            if (zeroTuneOffset) {
                mixedI = iSample;
                mixedQ = qSample;
            } else {
                mixedI = iSample * oscillatorI - qSample * oscillatorQ;
                mixedQ = iSample * oscillatorQ + qSample * oscillatorI;
            }
            audioChannelSumI += mixedI;
            audioChannelSumQ += mixedQ;
            pushAudioFirSample(mixedI, mixedQ);
            ++audioChannelDecimationCount;

            if (!zeroTuneOffset) {
                double nextOscillatorI = oscillatorI * stepI - oscillatorQ * stepQ;
                oscillatorQ = oscillatorI * stepQ + oscillatorQ * stepI;
                oscillatorI = nextOscillatorI;
                if ((sample & 0x1ff) == 0) {
                    double magnitude = oscillatorI * oscillatorI + oscillatorQ * oscillatorQ;
                    if (magnitude > 0.0) {
                        double scale = 1.0 / Math.sqrt(magnitude);
                        oscillatorI *= scale;
                        oscillatorQ *= scale;
                    }
                }
            }

            if (audioChannelDecimationCount < decimationFactor) {
                continue;
            }

            updateAudioFirOutput();
            double channelI = audioFirOutputI;
            double channelQ = audioFirOutputQ;
            if (audioFirTaps.length == 0) {
                channelI = audioChannelSumI / audioChannelDecimationCount;
                channelQ = audioChannelSumQ / audioChannelDecimationCount;
            }
            audioChannelSumI = 0.0;
            audioChannelSumQ = 0.0;
            audioChannelDecimationCount = 0;
            ++decimatedSamples;
            channelPeak = Math.max(channelPeak, Math.hypot(channelI, channelQ));

            audioChannelLowPassI += channelAlpha * (channelI - audioChannelLowPassI);
            audioChannelLowPassQ += channelAlpha * (channelQ - audioChannelLowPassQ);
            double demodulated = demodulateChannelSample(audioChannelLowPassI,
                    audioChannelLowPassQ,
                    modulationType,
                    channelRate);
            demodulated = applyAudioDeemphasis(demodulated, modulationType, channelRate);
            audioLowPassAverage += demodAlpha * (demodulated - audioLowPassAverage);
            demodulated = audioLowPassAverage;

            audioResamplePhase += outputStep;
            while (audioResamplePhase >= 1.0) {
                audioResamplePhase -= 1.0;
                double audioValue = normalizeAudioSample(demodulated, modulationType);
                int pcmValue = (int) Math.round(Math.max(-1.0, Math.min(1.0, audioValue)) * 32767.0);
                pcmPeak = Math.max(pcmPeak, Math.abs(pcmValue));
                if (output + 2 > pcm.length) {
                    byte[] grown = new byte[pcm.length * 2 + 64];
                    System.arraycopy(pcm, 0, grown, 0, output);
                    pcm = grown;
                }
                pcm[output++] = (byte) (pcmValue & 0xff);
                pcm[output++] = (byte) ((pcmValue >> 8) & 0xff);
            }
        }
        audioMixerPhase = zeroTuneOffset ? 0.0 : Math.atan2(oscillatorQ, oscillatorI);
        previewLastPcmPeak = pcmPeak;
        previewLastAudioOutputSamples = output / 2;
        previewLastAudioOffsetHz = tuneOffsetHz;

        long now = System.nanoTime();
        if (now - audioDebugLastLogNanos > 1_000_000_000L) {
            audioDebugLastLogNanos = now;
            Log.d(LOG_TAG, String.format(Locale.US,
                    "audio rtl raw inIq=%d decimated=%d out=%d sr=%.3fMHz timing=%.3fMHz playback=%.3fMHz off=%.0fHz zero=%s stride=%d dec=%d chRate=%.0f rawPeak=%.3f chPeak=%.5f pcmPeak=%d mod=%d",
                    complexSamples,
                    decimatedSamples,
                    output / 2,
                    nominalSampleRate / 1_000_000.0,
                    timingSampleRate / 1_000_000.0,
                    playbackSampleRate / 1_000_000.0,
                    tuneOffsetHz,
                    zeroTuneOffset,
                    inputStride,
                    decimationFactor,
                    channelRate,
                    rawPeak,
                    channelPeak,
                    pcmPeak,
                    modulationType));
        }
        if (output == pcm.length) {
            return pcm;
        }
        byte[] trimmed = new byte[output];
        System.arraycopy(pcm, 0, trimmed, 0, output);
        return trimmed;
    }

    private byte[] audioFrameFromRawFobosIq(byte[] buffer,
                                           int bytesRead,
                                           double centerFrequency,
                                           double listeningFrequency,
                                           double nominalSampleRate,
                                           double timingSampleRate,
                                           int inputMode,
                                           int modulationType) {
        int usableBytes = Math.max(0, Math.min(bytesRead, buffer.length)) & ~0x3;
        int complexSamples = usableBytes / 4;
        if (complexSamples <= 2 ||
                nominalSampleRate <= AUDIO_SAMPLE_RATE ||
                timingSampleRate <= AUDIO_SAMPLE_RATE) {
            return new byte[0];
        }

        double tuneOffsetHz = RadioSettings.isDirectInput(inputMode)
                ? listeningFrequency
                : listeningFrequency - centerFrequency;
        int inputStride = rawAudioInputStride(nominalSampleRate, modulationType);
        double effectiveSampleRate = nominalSampleRate / inputStride;
        double phaseStep = -2.0 * Math.PI * tuneOffsetHz / effectiveSampleRate;
        int decimationFactor = audioChannelDecimationFactor(effectiveSampleRate, modulationType, previewBandwidth);
        double channelRate = effectiveSampleRate / Math.max(1, decimationFactor);
        double channelCutoff = Math.min(audioChannelCutoffForMode(modulationType, previewBandwidth),
                channelRate * 0.45);
        double channelAlpha = clamp01(1.0 - Math.exp(-2.0 * Math.PI * channelCutoff / channelRate));
        double demodCutoff = Math.min(audioDemodCutoffForMode(modulationType, previewBandwidth),
                channelRate * 0.45);
        double demodAlpha = clamp01(1.0 - Math.exp(-2.0 * Math.PI * demodCutoff / channelRate));
        double outputStep = (AUDIO_SAMPLE_RATE * Math.max(1, decimationFactor)) / effectiveSampleRate;
        int estimatedOutputSamples = Math.max(8,
                (int) Math.ceil((complexSamples / nominalSampleRate) * AUDIO_SAMPLE_RATE) + 16);
        byte[] pcm = new byte[estimatedOutputSamples * 2];
        int output = 0;

        boolean directSampling = RadioSettings.isDirectInput(inputMode);
        int p0 = readLe16(buffer, 0);
        int p1 = usableBytes >= 4 ? readLe16(buffer, 2) : 0;
        boolean rawSwapIq = (p0 & 0x8000) != 0 && (p1 & 0x8000) != 0;
        boolean swapIq = directSampling || rawSwapIq != true;
        boolean agileSwappedHalves = false;
        int agileFirstOffset = 0;
        int agileSecondOffset = 0x4000;
        if (activeAgileApi) {
            agileSwappedHalves = (p0 & 0x4000) != 0 && (p1 & 0x4000) != 0 && usableBytes >= 0x8000;
            agileFirstOffset = p0 & p1 & 0x4000;
            agileSecondOffset = agileFirstOffset ^ 0x4000;
        }

        int decimatedSamples = 0;
        double oscillatorI = Math.cos(audioMixerPhase);
        double oscillatorQ = Math.sin(audioMixerPhase);
        double stepI = Math.cos(phaseStep);
        double stepQ = Math.sin(phaseStep);
        boolean cheapRawDecimator = effectiveSampleRate > 2_000_000.0;
        if (cheapRawDecimator) {
            disableAudioFir(decimationFactor, effectiveSampleRate, modulationType, previewBandwidth);
        } else {
            ensureAudioFirConfigured(decimationFactor, effectiveSampleRate, modulationType, previewBandwidth);
        }
        double rawScale = 1.0 / 8192.0;
        double dcRate = 0.0015;
        double rawPeak = 0.0;
        double channelPeak = 0.0;
        int pcmPeak = 0;

        for (int sample = 0; sample < complexSamples; sample += inputStride) {
            int pos = rawFobosSampleOffset(sample, usableBytes, agileSwappedHalves,
                    agileFirstOffset, agileSecondOffset);
            if (pos < 0 || pos + 3 >= usableBytes) {
                continue;
            }

            int first = readLe16(buffer, pos);
            int second = readLe16(buffer, pos + 2);
            double real = (swapIq ? second : first) & 0x3fff;
            double imag = (swapIq ? first : second) & 0x3fff;
            audioRawDcI += dcRate * (real - audioRawDcI);
            audioRawDcQ += dcRate * (imag - audioRawDcQ);
            double iSample = (real - audioRawDcI) * rawScale;
            double qSample = (imag - audioRawDcQ) * rawScale;
            rawPeak = Math.max(rawPeak, Math.max(Math.abs(iSample), Math.abs(qSample)));

            if (inputMode == RadioSettings.INPUT_HF_COMBINED) {
                if (tuneOffsetHz < 0.0) {
                    qSample = 0.0;
                } else {
                    iSample = qSample;
                    qSample = 0.0;
                }
            } else if (inputMode == RadioSettings.INPUT_HF1 ||
                    inputMode == RadioSettings.INPUT_HF_NOISE_CANCEL) {
                qSample = 0.0;
            } else if (inputMode == RadioSettings.INPUT_HF2) {
                iSample = qSample;
                qSample = 0.0;
            }

            double mixedI = iSample * oscillatorI - qSample * oscillatorQ;
            double mixedQ = iSample * oscillatorQ + qSample * oscillatorI;
            audioChannelSumI += mixedI;
            audioChannelSumQ += mixedQ;
            pushAudioFirSample(mixedI, mixedQ);
            ++audioChannelDecimationCount;

            if (audioChannelDecimationCount < decimationFactor) {
                double nextOscillatorI = oscillatorI * stepI - oscillatorQ * stepQ;
                oscillatorQ = oscillatorI * stepQ + oscillatorQ * stepI;
                oscillatorI = nextOscillatorI;
                if ((sample & 0x1ff) == 0) {
                    double magnitude = oscillatorI * oscillatorI + oscillatorQ * oscillatorQ;
                    if (magnitude > 0.0) {
                        double scale = 1.0 / Math.sqrt(magnitude);
                        oscillatorI *= scale;
                        oscillatorQ *= scale;
                    }
                }
                continue;
            }

            updateAudioFirOutput();
            double channelI = audioFirOutputI;
            double channelQ = audioFirOutputQ;
            if (audioFirTaps.length == 0) {
                channelI = audioChannelSumI / audioChannelDecimationCount;
                channelQ = audioChannelSumQ / audioChannelDecimationCount;
            }
            audioChannelSumI = 0.0;
            audioChannelSumQ = 0.0;
            audioChannelDecimationCount = 0;
            ++decimatedSamples;
            channelPeak = Math.max(channelPeak, Math.hypot(channelI, channelQ));

            double nextOscillatorI = oscillatorI * stepI - oscillatorQ * stepQ;
            oscillatorQ = oscillatorI * stepQ + oscillatorQ * stepI;
            oscillatorI = nextOscillatorI;
            if ((sample & 0x1ff) == 0) {
                double magnitude = oscillatorI * oscillatorI + oscillatorQ * oscillatorQ;
                if (magnitude > 0.0) {
                    double scale = 1.0 / Math.sqrt(magnitude);
                    oscillatorI *= scale;
                    oscillatorQ *= scale;
                }
            }

            audioChannelLowPassI += channelAlpha * (channelI - audioChannelLowPassI);
            audioChannelLowPassQ += channelAlpha * (channelQ - audioChannelLowPassQ);
            double demodulated = demodulateChannelSample(audioChannelLowPassI,
                    audioChannelLowPassQ,
                    modulationType,
                    channelRate);
            demodulated = applyAudioDeemphasis(demodulated, modulationType, channelRate);
            audioLowPassAverage += demodAlpha * (demodulated - audioLowPassAverage);
            demodulated = audioLowPassAverage;

            audioResamplePhase += outputStep;
            while (audioResamplePhase >= 1.0) {
                audioResamplePhase -= 1.0;
                double audioValue = normalizeAudioSample(demodulated, modulationType);
                int pcmValue = (int) Math.round(Math.max(-1.0, Math.min(1.0, audioValue)) * 32767.0);
                pcmPeak = Math.max(pcmPeak, Math.abs(pcmValue));
                if (output + 2 > pcm.length) {
                    byte[] grown = new byte[pcm.length * 2 + 64];
                    System.arraycopy(pcm, 0, grown, 0, output);
                    pcm = grown;
                }
                pcm[output++] = (byte) (pcmValue & 0xff);
                pcm[output++] = (byte) ((pcmValue >> 8) & 0xff);
            }
        }
        audioMixerPhase = Math.atan2(oscillatorQ, oscillatorI);
        previewLastPcmPeak = pcmPeak;
        previewLastAudioOutputSamples = output / 2;
        previewLastAudioOffsetHz = tuneOffsetHz;

        long now = System.nanoTime();
        if (now - audioDebugLastLogNanos > 1_000_000_000L) {
            audioDebugLastLogNanos = now;
            Log.d(LOG_TAG, String.format(Locale.US,
                    "audio raw inIq=%d decimated=%d out=%d sr=%.3fMHz timing=%.3fMHz off=%.0fHz dec=%d chRate=%.0f filter=%s rawPeak=%.3f chPeak=%.5f pcmPeak=%d mod=%d",
                    complexSamples,
                    decimatedSamples,
                    output / 2,
                    nominalSampleRate / 1_000_000.0,
                    timingSampleRate / 1_000_000.0,
                    tuneOffsetHz,
                    decimationFactor,
                    channelRate,
                    cheapRawDecimator ? "boxcar" : "fir",
                    rawPeak,
                    channelPeak,
                    pcmPeak,
                    modulationType));
        }
        if (output == pcm.length) {
            return pcm;
        }
        byte[] trimmed = new byte[output];
        System.arraycopy(pcm, 0, trimmed, 0, output);
        return trimmed;
    }

    private int rawFobosSampleOffset(int sample,
                                     int usableBytes,
                                     boolean agileSwappedHalves,
                                     int agileFirstOffset,
                                     int agileSecondOffset) {
        int directOffset = sample * 4;
        if (!agileSwappedHalves) {
            return directOffset;
        }
        int pairBytes = 0x8000;
        int halfBytes = 0x4000;
        int pairSamples = pairBytes / 4;
        int halfSamples = halfBytes / 4;
        int pair = sample / pairSamples;
        int withinPair = sample - pair * pairSamples;
        int pairBase = pair * pairBytes;
        if (pairBase + pairBytes > usableBytes) {
            return directOffset;
        }
        if (withinPair < halfSamples) {
            return pairBase + agileFirstOffset + withinPair * 4;
        }
        return pairBase + agileSecondOffset + (withinPair - halfSamples) * 4;
    }

    private double normalizeRadians(double phase) {
        if (!Double.isFinite(phase)) {
            return 0.0;
        }
        return Math.IEEEremainder(phase, 2.0 * Math.PI);
    }

    private byte[] audioFrameFromIq(float[] iqSamples,
                                    double centerFrequency,
                                    double listeningFrequency,
                                    double nominalSampleRate,
                                    double timingSampleRate,
                                    int inputMode,
                                    int modulationType) {
        int complexSamples = iqSamples.length / 2;
        if (complexSamples <= 2 ||
                nominalSampleRate <= AUDIO_SAMPLE_RATE ||
                timingSampleRate <= AUDIO_SAMPLE_RATE) {
            return new byte[0];
        }

        double tuneOffsetHz = RadioSettings.isDirectInput(inputMode)
                ? listeningFrequency
                : listeningFrequency - centerFrequency;
        int inputStride = audioInputStride(nominalSampleRate, modulationType);
        double effectiveSampleRate = nominalSampleRate / inputStride;
        double playbackSampleRate = Math.max(AUDIO_SAMPLE_RATE * 1.25,
                Math.min(nominalSampleRate, timingSampleRate)) / inputStride;
        double phaseStep = -2.0 * Math.PI * tuneOffsetHz / effectiveSampleRate;
        int decimationFactor = audioChannelDecimationFactor(effectiveSampleRate, modulationType, previewBandwidth);
        double channelRate = effectiveSampleRate / Math.max(1, decimationFactor);
        double channelCutoff = Math.min(audioChannelCutoffForMode(modulationType, previewBandwidth),
                channelRate * 0.45);
        double channelAlpha = clamp01(1.0 - Math.exp(-2.0 * Math.PI * channelCutoff / channelRate));
        double demodCutoff = Math.min(audioDemodCutoffForMode(modulationType, previewBandwidth),
                channelRate * 0.45);
        double demodAlpha = clamp01(1.0 - Math.exp(-2.0 * Math.PI * demodCutoff / channelRate));
        double outputStep = (AUDIO_SAMPLE_RATE * Math.max(1, decimationFactor)) / playbackSampleRate;
        int estimatedOutputSamples = Math.max(8,
                (int) Math.ceil((complexSamples / nominalSampleRate) * AUDIO_SAMPLE_RATE) + 16);
        byte[] pcm = new byte[estimatedOutputSamples * 2];
        int output = 0;
        double rawPeak = 0.0;
        double channelPeak = 0.0;
        int pcmPeak = 0;
        double oscillatorI = Math.cos(audioMixerPhase);
        double oscillatorQ = Math.sin(audioMixerPhase);
        double stepI = Math.cos(phaseStep);
        double stepQ = Math.sin(phaseStep);
        ensureAudioFirConfigured(decimationFactor, effectiveSampleRate, modulationType, previewBandwidth);

        for (int sample = 0; sample < complexSamples; sample += inputStride) {
            int base = sample * 2;
            double iSample = iqSamples[base];
            double qSample = iqSamples[base + 1];
            rawPeak = Math.max(rawPeak, Math.max(Math.abs(iSample), Math.abs(qSample)));

            if (inputMode == RadioSettings.INPUT_HF_COMBINED) {
                if (tuneOffsetHz < 0.0) {
                    qSample = 0.0;
                } else {
                    iSample = qSample;
                    qSample = 0.0;
                }
            } else if (inputMode == RadioSettings.INPUT_HF1 ||
                    inputMode == RadioSettings.INPUT_HF_NOISE_CANCEL) {
                qSample = 0.0;
            } else if (inputMode == RadioSettings.INPUT_HF2) {
                iSample = qSample;
                qSample = 0.0;
            }

            double mixedI = iSample * oscillatorI - qSample * oscillatorQ;
            double mixedQ = iSample * oscillatorQ + qSample * oscillatorI;
            audioChannelSumI += mixedI;
            audioChannelSumQ += mixedQ;
            pushAudioFirSample(mixedI, mixedQ);
            ++audioChannelDecimationCount;

            double nextOscillatorI = oscillatorI * stepI - oscillatorQ * stepQ;
            oscillatorQ = oscillatorI * stepQ + oscillatorQ * stepI;
            oscillatorI = nextOscillatorI;
            if ((sample & 0x1ff) == 0) {
                double magnitude = oscillatorI * oscillatorI + oscillatorQ * oscillatorQ;
                if (magnitude > 0.0) {
                    double scale = 1.0 / Math.sqrt(magnitude);
                    oscillatorI *= scale;
                    oscillatorQ *= scale;
                }
            }

            if (audioChannelDecimationCount < decimationFactor) {
                continue;
            }

            updateAudioFirOutput();
            double channelI = audioFirOutputI;
            double channelQ = audioFirOutputQ;
            if (audioFirTaps.length == 0) {
                channelI = audioChannelSumI / audioChannelDecimationCount;
                channelQ = audioChannelSumQ / audioChannelDecimationCount;
            }
            channelPeak = Math.max(channelPeak, Math.hypot(channelI, channelQ));
            audioChannelSumI = 0.0;
            audioChannelSumQ = 0.0;
            audioChannelDecimationCount = 0;

            audioChannelLowPassI += channelAlpha * (channelI - audioChannelLowPassI);
            audioChannelLowPassQ += channelAlpha * (channelQ - audioChannelLowPassQ);
            double demodulated = demodulateChannelSample(audioChannelLowPassI,
                    audioChannelLowPassQ,
                    modulationType,
                    channelRate);
            demodulated = applyAudioDeemphasis(demodulated, modulationType, channelRate);
            audioLowPassAverage += demodAlpha * (demodulated - audioLowPassAverage);
            demodulated = audioLowPassAverage;

            audioResamplePhase += outputStep;
            while (audioResamplePhase >= 1.0) {
                audioResamplePhase -= 1.0;
                double audioValue = normalizeAudioSample(demodulated, modulationType);
                int pcmValue = (int) Math.round(Math.max(-1.0, Math.min(1.0, audioValue)) * 32767.0);
                pcmPeak = Math.max(pcmPeak, Math.abs(pcmValue));
                if (output + 2 > pcm.length) {
                    byte[] grown = new byte[pcm.length * 2 + 64];
                    System.arraycopy(pcm, 0, grown, 0, output);
                    pcm = grown;
                }
                pcm[output++] = (byte) (pcmValue & 0xff);
                pcm[output++] = (byte) ((pcmValue >> 8) & 0xff);
            }
        }
        audioMixerPhase = Math.atan2(oscillatorQ, oscillatorI);
        previewLastPcmPeak = pcmPeak;
        previewLastAudioOutputSamples = output / 2;
        previewLastAudioOffsetHz = tuneOffsetHz;
        long now = System.nanoTime();
        if (now - audioDebugLastLogNanos > 1_000_000_000L) {
            audioDebugLastLogNanos = now;
            Log.d(LOG_TAG, String.format(Locale.US,
                    "audio frame inIq=%d out=%d sr=%.3fMHz timing=%.3fMHz off=%.0fHz dec=%d chRate=%.0f rawPeak=%.3f chPeak=%.5f pcmPeak=%d mod=%d",
                    complexSamples,
                    output / 2,
                    nominalSampleRate / 1_000_000.0,
                    timingSampleRate / 1_000_000.0,
                    tuneOffsetHz,
                    decimationFactor,
                    channelRate,
                    rawPeak,
                    channelPeak,
                    pcmPeak,
                    modulationType));
        }
        if (output == pcm.length) {
            return pcm;
        }
        byte[] trimmed = new byte[output];
        System.arraycopy(pcm, 0, trimmed, 0, output);
        return trimmed;
    }

    private void ensureAudioFirConfigured(int decimationFactor,
                                          double sampleRate,
                                          int modulationType,
                                          double bandwidth) {
        int normalizedDecimation = Math.max(1, decimationFactor);
        if (audioFirTaps.length > 0 &&
                audioFirConfiguredDecimation == normalizedDecimation &&
                audioFirConfiguredModulation == modulationType &&
                Math.abs(audioFirConfiguredSampleRate - sampleRate) < 1.0 &&
                Math.abs(audioFirConfiguredBandwidth - bandwidth) < 1.0) {
            return;
        }

        int tapCount = audioFirTapCount(normalizedDecimation, modulationType);
        if (tapCount <= 0) {
            audioFirTaps = new double[0];
            audioFirRingI = new double[0];
            audioFirRingQ = new double[0];
            audioFirRingIndex = 0;
            audioChannelDecimationCount = 0;
            audioFirConfiguredDecimation = normalizedDecimation;
            audioFirConfiguredModulation = modulationType;
            audioFirConfiguredSampleRate = sampleRate;
            audioFirConfiguredBandwidth = bandwidth;
            return;
        }
        double outputNyquist = sampleRate / (2.0 * normalizedDecimation);
        double cutoff = Math.min(audioChannelCutoffForMode(modulationType, bandwidth) * 1.15,
                outputNyquist * 0.82);
        cutoff = Math.max(500.0, Math.min(cutoff, sampleRate * 0.45));

        double[] taps = new double[tapCount];
        double normalizedCutoff = cutoff / sampleRate;
        int center = tapCount / 2;
        double sum = 0.0;
        for (int i = 0; i < tapCount; ++i) {
            int m = i - center;
            double sinc = m == 0
                    ? 2.0 * normalizedCutoff
                    : Math.sin(2.0 * Math.PI * normalizedCutoff * m) / (Math.PI * m);
            double window = 0.42
                    - 0.5 * Math.cos((2.0 * Math.PI * i) / (tapCount - 1))
                    + 0.08 * Math.cos((4.0 * Math.PI * i) / (tapCount - 1));
            taps[i] = sinc * window;
            sum += taps[i];
        }
        if (Math.abs(sum) > 1.0e-12) {
            for (int i = 0; i < taps.length; ++i) {
                taps[i] /= sum;
            }
        }

        audioFirTaps = taps;
        audioFirRingI = new double[tapCount];
        audioFirRingQ = new double[tapCount];
        audioFirRingIndex = 0;
        audioChannelDecimationCount = 0;
        audioFirConfiguredDecimation = normalizedDecimation;
        audioFirConfiguredModulation = modulationType;
        audioFirConfiguredSampleRate = sampleRate;
        audioFirConfiguredBandwidth = bandwidth;
    }

    private void disableAudioFir(int decimationFactor,
                                 double sampleRate,
                                 int modulationType,
                                 double bandwidth) {
        int normalizedDecimation = Math.max(1, decimationFactor);
        if (audioFirTaps.length == 0 &&
                audioFirConfiguredDecimation == normalizedDecimation &&
                audioFirConfiguredModulation == modulationType &&
                Math.abs(audioFirConfiguredSampleRate - sampleRate) < 1.0 &&
                Math.abs(audioFirConfiguredBandwidth - bandwidth) < 1.0) {
            return;
        }
        audioFirTaps = new double[0];
        audioFirRingI = new double[0];
        audioFirRingQ = new double[0];
        audioFirRingIndex = 0;
        audioChannelDecimationCount = 0;
        audioFirConfiguredDecimation = normalizedDecimation;
        audioFirConfiguredModulation = modulationType;
        audioFirConfiguredSampleRate = sampleRate;
        audioFirConfiguredBandwidth = bandwidth;
        audioFirOutputI = 0.0;
        audioFirOutputQ = 0.0;
    }

    private int audioFirTapCount(int decimationFactor, int modulationType) {
        int taps;
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                taps = decimationFactor * 4 + 1;
                taps = Math.max(41, Math.min(81, taps));
                break;
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_DMR:
                taps = decimationFactor * 5 + 1;
                taps = Math.max(49, Math.min(97, taps));
                break;
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
            case RadioSettings.MOD_CW:
                taps = decimationFactor * 5 + 1;
                taps = Math.max(49, Math.min(97, taps));
                break;
            default:
                taps = decimationFactor * 4 + 1;
                taps = Math.max(33, Math.min(81, taps));
                break;
        }
        return (taps & 1) == 0 ? taps + 1 : taps;
    }

    private void pushAudioFirSample(double sampleI, double sampleQ) {
        if (audioFirRingI.length == 0) {
            return;
        }
        audioFirRingIndex = (audioFirRingIndex + 1) % audioFirRingI.length;
        audioFirRingI[audioFirRingIndex] = sampleI;
        audioFirRingQ[audioFirRingIndex] = sampleQ;
    }

    private void updateAudioFirOutput() {
        if (audioFirTaps.length == 0 || audioFirRingI.length != audioFirTaps.length) {
            audioFirOutputI = 0.0;
            audioFirOutputQ = 0.0;
            return;
        }
        double sumI = 0.0;
        double sumQ = 0.0;
        int ringLength = audioFirRingI.length;
        int index = audioFirRingIndex;
        for (int tap = 0; tap < audioFirTaps.length; ++tap) {
            double coefficient = audioFirTaps[tap];
            sumI += audioFirRingI[index] * coefficient;
            sumQ += audioFirRingQ[index] * coefficient;
            --index;
            if (index < 0) {
                index = ringLength - 1;
            }
        }
        audioFirOutputI = sumI;
        audioFirOutputQ = sumQ;
    }

    private double applyAudioDeemphasis(double demodulated, int modulationType, double channelRate) {
        if (modulationType != RadioSettings.MOD_WFM || channelRate <= 0.0) {
            return demodulated;
        }
        double tauSeconds = 50.0e-6;
        double alpha = clamp01(1.0 - Math.exp(-1.0 / (tauSeconds * channelRate)));
        audioDeemphasisAverage += alpha * (demodulated - audioDeemphasisAverage);
        return audioDeemphasisAverage;
    }

    private double demodulateChannelSample(double channelI,
                                           double channelQ,
                                           int modulationType,
                                           double channelRate) {
        switch (modulationType) {
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_WFM:
            case RadioSettings.MOD_DMR:
                double magnitude = Math.hypot(channelI, channelQ);
                if (magnitude < 1.0e-9) {
                    return 0.0;
                }
                channelI /= magnitude;
                channelQ /= magnitude;
                if (!audioFmPhaseValid) {
                    audioFmLastI = channelI;
                    audioFmLastQ = channelQ;
                    audioFmPhaseValid = true;
                    return 0.0;
                }
                double cross = audioFmLastI * channelQ - audioFmLastQ * channelI;
                double power = audioFmLastI * audioFmLastI + audioFmLastQ * audioFmLastQ + 1.0e-12;
                double diff = cross / power;
                audioFmLastI = channelI;
                audioFmLastQ = channelQ;
                if (modulationType == RadioSettings.MOD_DMR) {
                    return diff * 10.0;
                }
                double deviationHz = modulationType == RadioSettings.MOD_WFM ? 75_000.0 : 5_000.0;
                double rate = Math.max(AUDIO_SAMPLE_RATE, channelRate);
                return diff * rate / (2.0 * Math.PI * deviationHz);
            case RadioSettings.MOD_AM:
            case RadioSettings.MOD_SAM:
                return Math.hypot(channelI, channelQ);
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
            case RadioSettings.MOD_DSB:
            case RadioSettings.MOD_CW:
                return channelI;
            default:
                return 0.0;
        }
    }

    private double normalizeAudioSample(double demodulated, int modulationType) {
        boolean digital = modulationType == RadioSettings.MOD_DMR;
        boolean fm = modulationType == RadioSettings.MOD_WFM || modulationType == RadioSettings.MOD_NFM;
        double dcRate = digital ? 0.0001 : (fm ? 0.0008 : 0.0005);
        audioDcAverage += (demodulated - audioDcAverage) * dcRate;
        double acSample = demodulated - audioDcAverage;
        double absSample = Math.abs(acSample);
        double agcRate = absSample > audioAgcLevel ? (fm ? 0.015 : 0.04) : 0.0002;
        audioAgcLevel += (absSample - audioAgcLevel) * agcRate;
        audioAgcLevel = Math.max(audioAgcLevel, 0.0001);
        double target = digital ? 0.25 : (modulationType == RadioSettings.MOD_WFM ? 0.24 :
                (modulationType == RadioSettings.MOD_NFM ? 0.30 : 0.28));
        double normalized = acSample * (target / audioAgcLevel);
        return digital ? normalized : Math.tanh(normalized * (fm ? 0.58 : 1.0));
    }

    private int audioChannelDecimationFactor(double sampleRate, int modulationType, double bandwidth) {
        double targetRate = audioTargetChannelRate(modulationType, bandwidth);
        int factor = Math.max(1, (int) Math.floor(sampleRate / Math.max(1.0, targetRate)));
        if (sampleRate > 1_000_000.0) {
            int minimum;
            switch (modulationType) {
                case RadioSettings.MOD_WFM:
                    minimum = 2;
                    break;
                case RadioSettings.MOD_NFM:
                case RadioSettings.MOD_DMR:
                    minimum = 3;
                    break;
                default:
                    minimum = 2;
                    break;
            }
            factor = Math.max(minimum, factor);
        }
        return factor;
    }

    private int audioInputStride(double sampleRate, int modulationType) {
        double targetInputRate;
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                targetInputRate = 256_000.0;
                break;
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_DMR:
                targetInputRate = 384_000.0;
                break;
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
            case RadioSettings.MOD_CW:
                targetInputRate = 192_000.0;
                break;
            default:
                targetInputRate = 256_000.0;
                break;
        }
        if (sampleRate <= targetInputRate * 1.4) {
            return 1;
        }
        int stride = (int) Math.floor(sampleRate / targetInputRate);
        return Math.max(1, Math.min(32, stride));
    }

    private int audioInputStrideForOffset(double sampleRate,
                                          int modulationType,
                                          double bandwidth,
                                          double tuneOffsetHz) {
        int preferredStride = audioInputStride(sampleRate, modulationType);
        if (preferredStride <= 1 || !Double.isFinite(sampleRate) || sampleRate <= 0.0 ||
                !Double.isFinite(tuneOffsetHz)) {
            return 1;
        }
        double channelCutoff = audioChannelCutoffForMode(modulationType, bandwidth);
        double guardHz = Math.abs(tuneOffsetHz) + Math.max(channelCutoff, 1_000.0);
        if (!Double.isFinite(guardHz) || guardHz <= 0.0) {
            return preferredStride;
        }
        int safeStride = (int) Math.floor(sampleRate / Math.max(1.0, guardHz * 2.0));
        return Math.max(1, Math.min(preferredStride, safeStride));
    }

    private int rawAudioInputStride(double sampleRate, int modulationType) {
        if (sampleRate <= PREVIEW_MAX_SAMPLE_RATE + 1.0) {
            return 1;
        }
        return audioInputStride(sampleRate, modulationType);
    }

    private double audioTargetChannelRate(int modulationType, double bandwidth) {
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                return Math.max(384_000.0, Math.min(768_000.0, bandwidth * 3.0));
            case RadioSettings.MOD_DMR:
                return 96_000.0;
            case RadioSettings.MOD_NFM:
                return Math.max(240_000.0, Math.min(384_000.0, bandwidth * 4.0));
            default:
                return 192_000.0;
        }
    }

    private double audioChannelCutoffForMode(int modulationType, double bandwidth) {
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                return Math.min(140_000.0, Math.max(80_000.0, bandwidth * 0.55));
            case RadioSettings.MOD_NFM:
                return Math.min(25_000.0, Math.max(6_000.0, bandwidth * 0.55));
            case RadioSettings.MOD_DMR:
                return Math.min(9_500.0, Math.max(6_000.0, bandwidth * 0.75));
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
                return Math.min(3_600.0, Math.max(700.0, bandwidth * 0.95));
            case RadioSettings.MOD_CW:
                return Math.min(1_500.0, Math.max(250.0, bandwidth * 0.5));
            case RadioSettings.MOD_DSB:
            case RadioSettings.MOD_SAM:
                return Math.min(12_000.0, Math.max(1_000.0, bandwidth * 0.45));
            case RadioSettings.MOD_AM:
            default:
                return Math.min(10_000.0, Math.max(1_000.0, bandwidth * 0.45));
        }
    }

    private double audioDemodCutoffForMode(int modulationType, double bandwidth) {
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                return 15_000.0;
            case RadioSettings.MOD_NFM:
                return 3_000.0;
            case RadioSettings.MOD_DMR:
                return 6_000.0;
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
                return Math.min(3_600.0, Math.max(700.0, bandwidth * 0.95));
            case RadioSettings.MOD_CW:
                return 1_200.0;
            case RadioSettings.MOD_DSB:
            case RadioSettings.MOD_SAM:
                return Math.min(6_500.0, Math.max(1_000.0, bandwidth * 0.45));
            case RadioSettings.MOD_AM:
            default:
                return Math.min(6_000.0, Math.max(1_000.0, bandwidth * 0.45));
        }
    }

    private double clamp01(double value) {
        if (!Double.isFinite(value)) {
            return 0.0;
        }
        return Math.max(0.0, Math.min(1.0, value));
    }

    private int audioTapCount(double sourceStep, double sampleRate, int modulationType) {
        if (!Double.isFinite(sourceStep) || sourceStep <= 1.0 || sampleRate <= 0.0) {
            return 8;
        }
        double channelHz;
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                channelHz = 220_000.0;
                break;
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_DMR:
                channelHz = Math.max(9_000.0, Math.min(20_000.0, previewBandwidth));
                break;
            case RadioSettings.MOD_AM:
            case RadioSettings.MOD_SAM:
            case RadioSettings.MOD_DSB:
                channelHz = Math.max(6_000.0, Math.min(18_000.0, previewBandwidth * 1.6));
                break;
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
            case RadioSettings.MOD_CW:
                channelHz = Math.max(2_400.0, Math.min(6_000.0, previewBandwidth * 1.8));
                break;
            default:
                channelHz = Math.max(8_000.0, Math.min(20_000.0, previewBandwidth));
                break;
        }
        int taps = (int) Math.round((sampleRate / channelHz) * 2.2);
        return Math.max(12, Math.min(256, taps));
    }

    private double demodulateAudioSample(double mixedI, double mixedQ, int modulationType) {
        switch (modulationType) {
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_WFM:
            case RadioSettings.MOD_DMR:
                double phase = Math.atan2(mixedQ, mixedI);
                if (!audioFmPhaseValid) {
                    audioFmLastPhase = phase;
                    audioFmPhaseValid = true;
                    return 0.0;
                }
                double diff = wrapPhase(phase - audioFmLastPhase);
                audioFmLastPhase = phase;
                double fmGain = modulationType == RadioSettings.MOD_WFM ? 0.12 : 0.55;
                return audioLowPass(removeAudioDc(diff * fmGain, 0.0008), modulationType);
            case RadioSettings.MOD_AM:
            case RadioSettings.MOD_SAM:
                double envelope = Math.hypot(mixedI, mixedQ);
                audioEnvelopeAverage += (envelope - audioEnvelopeAverage) * 0.002;
                return audioLowPass(removeAudioDc((envelope - audioEnvelopeAverage) * 2.4, 0.0015),
                        modulationType);
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
            case RadioSettings.MOD_DSB:
            case RadioSettings.MOD_CW:
                return audioLowPass(removeAudioDc(mixedI * 1.4, 0.001), modulationType);
            default:
                return 0.0;
        }
    }

    private double audioLowPass(double value, int modulationType) {
        double cutoffHz;
        switch (modulationType) {
            case RadioSettings.MOD_WFM:
                cutoffHz = 15_000.0;
                break;
            case RadioSettings.MOD_NFM:
            case RadioSettings.MOD_DMR:
                cutoffHz = 3_600.0;
                break;
            case RadioSettings.MOD_CW:
                cutoffHz = 900.0;
                break;
            case RadioSettings.MOD_USB:
            case RadioSettings.MOD_LSB:
                cutoffHz = 3_000.0;
                break;
            default:
                cutoffHz = 5_000.0;
                break;
        }
        double alpha = 1.0 - Math.exp(-2.0 * Math.PI * cutoffHz / AUDIO_SAMPLE_RATE);
        audioLowPassAverage += (value - audioLowPassAverage) * Math.max(0.0, Math.min(1.0, alpha));
        return audioLowPassAverage;
    }

    private double removeAudioDc(double value, double rate) {
        audioDcAverage += (value - audioDcAverage) * Math.max(0.0, Math.min(1.0, rate));
        return value - audioDcAverage;
    }

    private double wrapPhase(double phase) {
        while (phase > Math.PI) {
            phase -= 2.0 * Math.PI;
        }
        while (phase < -Math.PI) {
            phase += 2.0 * Math.PI;
        }
        return phase;
    }

    private FobosNetworkClient.SpectrumFrame spectrumFrameFromIq(float[] iqSamples,
                                                                 double centerFrequency,
                                                                 double listeningFrequency,
                                                                 double sampleRate,
                                                                 int inputMode,
                                                                 int fftLength,
                                                                 double bandwidth,
                                                                 int modulationType,
                                                                 int requestedFftLength) {
        int availableComplexSamples = iqSamples.length / 2;
        if (availableComplexSamples < fftLength) {
            return null;
        }

        float[] real = new float[fftLength];
        float[] imag = new float[fftLength];
        int sourceStart = Math.max(0, availableComplexSamples - fftLength);
        for (int i = 0; i < fftLength; ++i) {
            int base = (sourceStart + i) * 2;
            double window = 0.5 - 0.5 * Math.cos((2.0 * Math.PI * i) / Math.max(1, fftLength - 1));
            real[i] = (float) (iqSamples[base] * window);
            imag[i] = (float) (iqSamples[base + 1] * window);
        }

        fftForward(real, imag);

        double displayCenter = inputMode == RadioSettings.INPUT_RF ? centerFrequency : 0.0;
        double minFrequency = displayCenter - sampleRate * 0.5;
        double maxFrequency = displayCenter + sampleRate * 0.5;
        if (inputMode == RadioSettings.INPUT_HF1 || inputMode == RadioSettings.INPUT_HF2) {
            minFrequency = 0.0;
            maxFrequency = sampleRate * 0.5;
        }

        double[] frequencies = new double[fftLength];
        float[] magnitudes = new float[fftLength];
        double binWidth = (maxFrequency - minFrequency) / Math.max(1, fftLength - 1);
        double normalizer = 1.0 / Math.max(1, fftLength);
        for (int i = 0; i < fftLength; ++i) {
            int shifted = (i + fftLength / 2) % fftLength;
            double magnitude = Math.hypot(real[shifted], imag[shifted]) * normalizer;
            double db = 20.0 * Math.log10(Math.max(1.0e-12, magnitude)) + 84.0;
            frequencies[i] = minFrequency + binWidth * i;
            magnitudes[i] = (float) Math.max(-180.0, Math.min(90.0, db));
        }

        return new FobosNetworkClient.SpectrumFrame(
                frequencies,
                magnitudes,
                minFrequency,
                maxFrequency,
                displayCenter,
                listeningFrequency,
                sampleRate,
                bandwidth,
                modulationType,
                inputMode,
                requestedFftLength,
                true);
    }

    private void fftForward(float[] real, float[] imag) {
        int n = real.length;
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                float tempReal = real[i];
                real[i] = real[j];
                real[j] = tempReal;
                float tempImag = imag[i];
                imag[i] = imag[j];
                imag[j] = tempImag;
            }
        }

        for (int len = 2; len <= n; len <<= 1) {
            double angle = -2.0 * Math.PI / len;
            float wLenReal = (float) Math.cos(angle);
            float wLenImag = (float) Math.sin(angle);
            int half = len >> 1;
            for (int start = 0; start < n; start += len) {
                float wReal = 1.0f;
                float wImag = 0.0f;
                for (int j = 0; j < half; ++j) {
                    int even = start + j;
                    int odd = even + half;
                    float oddReal = real[odd] * wReal - imag[odd] * wImag;
                    float oddImag = real[odd] * wImag + imag[odd] * wReal;
                    real[odd] = real[even] - oddReal;
                    imag[odd] = imag[even] - oddImag;
                    real[even] += oddReal;
                    imag[even] += oddImag;

                    float nextReal = wReal * wLenReal - wImag * wLenImag;
                    wImag = wReal * wLenImag + wImag * wLenReal;
                    wReal = nextReal;
                }
            }
        }
    }

    private String formatMhz(double valueHz) {
        return String.format(Locale.US, "%.3f", valueHz / 1_000_000.0);
    }

    private void closeActiveSession() {
        stopPreviewThread(700L);
        UsbDeviceConnection connection = activeConnection;
        UsbInterface usbInterface = activeInterface;
        activeReceiverKind = ActiveUsbReceiverKind.NONE;
        rtlBasebandInitialized = false;
        rtlLastSampleRate = 0;
        rtlLastCenterFrequency = 0;
        rtlTunerI2cAddress = 0;
        rtlTunerName = "";
        rtlR82xxInput = -1;
        rtlR82xxHasLock = false;
        activeConnection = null;
        activeDevice = null;
        activeInterface = null;
        activeBulkInEndpoint = null;
        activeBulkOutEndpoint = null;
        if (connection == null) {
            return;
        }
        if (usbInterface != null) {
            try {
                connection.releaseInterface(usbInterface);
            } catch (RuntimeException ignored) {
                // Android USB stacks differ; close is the real cleanup boundary.
            }
        }
        connection.close();
    }

    private boolean isFobosCandidate(UsbDevice device) {
        return device.getVendorId() == FOBOS_VENDOR_ID &&
                device.getProductId() == FOBOS_PRODUCT_ID;
    }

    private boolean isRtlSdrCandidate(UsbDevice device) {
        int vendorId = device.getVendorId();
        int productId = device.getProductId();
        if (vendorId == REALTEK_VENDOR_ID) {
            return productId == RTL2832_PRODUCT_ID || productId == RTL2838_PRODUCT_ID;
        }
        if (vendorId == TERRATEC_VENDOR_ID) {
            return productId == TERRATEC_RTL_E4000_PRODUCT_ID ||
                    productId == TERRATEC_RTL_R820T_PRODUCT_ID;
        }
        return false;
    }

    private boolean isKnownReceiverCandidate(UsbDevice device) {
        return isFobosCandidate(device) || isRtlSdrCandidate(device);
    }

    private int devicePriority(UsbDevice device) {
        int target = preferredReceiverTarget;
        if (target == USB_TARGET_RTL_SDR) {
            if (isRtlSdrCandidate(device)) {
                return 0;
            }
            if (isFobosCandidate(device)) {
                return 1;
            }
            return 2;
        }
        if (target == USB_TARGET_FOBOS) {
            if (isFobosCandidate(device)) {
                return 0;
            }
            if (isRtlSdrCandidate(device)) {
                return 1;
            }
            return 2;
        }
        if (isFobosCandidate(device)) {
            return 0;
        }
        if (isRtlSdrCandidate(device)) {
            return 1;
        }
        return 2;
    }

    private String receiverKindLabel(UsbDevice device) {
        if (isFobosCandidate(device)) {
            return "Fobos SDR";
        }
        if (isRtlSdrCandidate(device)) {
            return "RTL-SDR";
        }
        return "USB";
    }

    private String receiverTargetLabel(int target) {
        if (target == USB_TARGET_RTL_SDR) {
            return "RTL-SDR";
        }
        if (target == USB_TARGET_FOBOS) {
            return "Fobos";
        }
        return "Auto";
    }

    private String fobosApiHint(byte[] rawDescriptors) {
        int bcdDevice = bcdDeviceFromRawDescriptors(rawDescriptors);
        if (bcdDevice == FOBOS_AGILE_BCD_DEVICE) {
            return "agile API expected";
        }
        if (bcdDevice == FOBOS_STANDARD_BCD_DEVICE) {
            return "standard API expected";
        }
        if (bcdDevice >= 0) {
            return "unknown bcdDevice " + formatWord(bcdDevice);
        }
        return "bcdDevice unavailable";
    }

    private int bcdDeviceFromRawDescriptors(byte[] rawDescriptors) {
        if (rawDescriptors == null || rawDescriptors.length < 14) {
            return -1;
        }
        return (rawDescriptors[12] & 0xff) | ((rawDescriptors[13] & 0xff) << 8);
    }

    private String shortDeviceName(UsbDevice device) {
        return String.format(Locale.US,
                "%s vid:pid %s:%s version %s%s",
                device.getDeviceName(),
                formatWord(device.getVendorId()),
                formatWord(device.getProductId()),
                safeVersion(device),
                isFobosCandidate(device) ? " (Fobos)" :
                        (isRtlSdrCandidate(device) ? " (RTL-SDR)" : ""));
    }

    private String safeVersion(UsbDevice device) {
        try {
            String value = device.getVersion();
            return value != null ? value : "?";
        } catch (RuntimeException e) {
            return "?";
        }
    }

    private String safeManufacturer(UsbDevice device) {
        try {
            String value = device.getManufacturerName();
            return value != null ? value : "?";
        } catch (SecurityException e) {
            return "<permission needed>";
        }
    }

    private String safeProduct(UsbDevice device) {
        try {
            String value = device.getProductName();
            return value != null ? value : "?";
        } catch (SecurityException e) {
            return "<permission needed>";
        }
    }

    private String safeSerial(UsbDevice device) {
        try {
            String value = device.getSerialNumber();
            return value != null ? value : "?";
        } catch (SecurityException e) {
            return "<permission needed>";
        }
    }

    private String endpointDirection(UsbEndpoint endpoint) {
        return endpoint.getDirection() == UsbConstants.USB_DIR_IN ? "IN" : "OUT";
    }

    private String endpointType(UsbEndpoint endpoint) {
        switch (endpoint.getType()) {
            case UsbConstants.USB_ENDPOINT_XFER_CONTROL:
                return "control";
            case UsbConstants.USB_ENDPOINT_XFER_ISOC:
                return "iso";
            case UsbConstants.USB_ENDPOINT_XFER_BULK:
                return "bulk";
            case UsbConstants.USB_ENDPOINT_XFER_INT:
                return "interrupt";
            default:
                return "type " + endpoint.getType();
        }
    }

    private String endpointSummary(UsbEndpoint endpoint) {
        if (endpoint == null) {
            return "none";
        }
        return endpointDirection(endpoint) + " " +
                endpointType(endpoint) +
                " addr " + formatByte(endpoint.getAddress()) +
                " maxPacket " + endpoint.getMaxPacketSize();
    }

    private String hexPreview(byte[] buffer, int length, int limit) {
        int count = Math.max(0, Math.min(Math.min(length, buffer.length), limit));
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < count; ++i) {
            if (i > 0) {
                builder.append(' ');
            }
            builder.append(formatByte(buffer[i]));
        }
        if (length > count) {
            builder.append(" ...");
        }
        return builder.toString();
    }

    private String formatByte(int value) {
        return String.format(Locale.US, "0x%02x", value & 0xff);
    }

    private String formatWord(int value) {
        return String.format(Locale.US, "0x%04x", value & 0xffff);
    }

    private void log(String message) {
        if (listener != null) {
            listener.onUsbLog(message);
        } else {
            Log.d(LOG_TAG, message != null ? message : "<null USB sandbox message>");
        }
    }

    private static final class StreamInterfaceChoice {
        final int interfaceIndex;
        final UsbInterface usbInterface;
        final UsbEndpoint bulkInEndpoint;
        final UsbEndpoint bulkOutEndpoint;

        StreamInterfaceChoice(
                int interfaceIndex,
                UsbInterface usbInterface,
                UsbEndpoint bulkInEndpoint,
                UsbEndpoint bulkOutEndpoint) {
            this.interfaceIndex = interfaceIndex;
            this.usbInterface = usbInterface;
            this.bulkInEndpoint = bulkInEndpoint;
            this.bulkOutEndpoint = bulkOutEndpoint;
        }
    }

    private static final class ControlStringResult {
        final int bytesRead;
        final String value;

        ControlStringResult(int bytesRead, String value) {
            this.bytesRead = bytesRead;
            this.value = value;
        }
    }
}
