package com.fobosapp.networkclient;

import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.BufferedInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

final class FobosNetworkClient {
    interface Listener {
        void onStatus(String status);
        void onDisconnected(String reason);
        void onRole(boolean canControl, String peerId, String peerLabel, String controllerPeerId);
        void onSpectrum(SpectrumFrame frame);
        void onAudio(byte[] pcmData);
        void onControlMessage(JSONObject command);
        default void onAudioReset() { }
    }

    static final class SpectrumFrame {
        final double[] frequencies;
        final float[] magnitudes;
        final double minFrequency;
        final double maxFrequency;
        final double centerFrequency;
        final double listeningFrequency;
        final double sampleRate;
        final double bandwidth;
        final int modulationType;
        final int inputMode;
        final int requestedFftLength;
        final boolean localUsb;

        SpectrumFrame(double[] frequencies,
                      float[] magnitudes,
                      double minFrequency,
                      double maxFrequency,
                      double centerFrequency,
                      double listeningFrequency,
                      double sampleRate,
                      double bandwidth,
                      int modulationType,
                      int inputMode) {
            this(frequencies, magnitudes, minFrequency, maxFrequency, centerFrequency,
                    listeningFrequency, sampleRate, bandwidth, modulationType, inputMode, 0, false);
        }

        SpectrumFrame(double[] frequencies,
                      float[] magnitudes,
                      double minFrequency,
                      double maxFrequency,
                      double centerFrequency,
                      double listeningFrequency,
                      double sampleRate,
                      double bandwidth,
                      int modulationType,
                      int inputMode,
                      int requestedFftLength,
                      boolean localUsb) {
            this.frequencies = frequencies;
            this.magnitudes = magnitudes;
            this.minFrequency = minFrequency;
            this.maxFrequency = maxFrequency;
            this.centerFrequency = centerFrequency;
            this.listeningFrequency = listeningFrequency;
            this.sampleRate = sampleRate;
            this.bandwidth = bandwidth;
            this.modulationType = modulationType;
            this.inputMode = inputMode;
            this.requestedFftLength = requestedFftLength;
            this.localUsb = localUsb;
        }
    }

    private static final String HELLO = "FOBOSAPP_HELLO 1";
    private static final String OK = "FOBOSAPP_OK 1";
    private static final String PING = "FOBOSAPP_PING 1";
    private static final String PONG = "FOBOSAPP_PONG 1";
    private static final int CONNECT_TIMEOUT_MS = 4000;
    private static final int MAX_LINE_BYTES = 8 * 1024 * 1024;
    private static final long MAX_BINARY_BYTES = 512L * 1024L * 1024L;

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final ExecutorService writerExecutor = Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService retryExecutor = Executors.newSingleThreadScheduledExecutor();
    private final Object writeLock = new Object();
    private final Listener listener;
    private final NetworkControlProtocol controlProtocol = new NetworkControlProtocol();
    private final RemoteVfoProtocol remoteProtocol = new RemoteVfoProtocol();
    private volatile Socket socket;
    private OutputStream outputStream;
    private volatile boolean running;
    private volatile boolean controlReady;
    private volatile boolean localClientHasControl;
    private volatile boolean connectionRequested;
    private volatile boolean autoReconnect = true;
    private volatile boolean remoteVfoMode;
    private volatile long generation;
    private ScheduledFuture<?> retry;
    private ScheduledFuture<?> vfoDeadline;
    private int retryCount;
    private final int retryBaseMs;
    private String host;
    private int port;

    FobosNetworkClient(Listener listener) {
        this(listener, 1000);
    }

    FobosNetworkClient(Listener listener, int retryBaseMs) {
        this.listener = listener;
        this.retryBaseMs = Math.max(20, retryBaseMs);
    }

    boolean isConnected() {
        Socket current = socket;
        return current != null && current.isConnected() && !current.isClosed() && controlReady;
    }

    boolean isActive() {
        return connectionRequested;
    }

    boolean canControl() {
        return remoteVfoMode ? remoteProtocol.isReady() : localClientHasControl;
    }

    boolean isRemoteVfoMode() { return remoteVfoMode; }
    void setRemoteVfoMode(boolean enabled) {
        if (isActive()) throw new IllegalStateException("Disconnect before changing network mode");
        remoteVfoMode = enabled;
    }
    void setAutoReconnect(boolean enabled) {
        autoReconnect = enabled;
        synchronized (writeLock) {
            if (!enabled && retry != null) disconnect();
        }
    }
    void selectVfo(String id) { remoteProtocol.select(id); listener.onAudioReset(); }
    JSONObject selectedVfo() { return remoteProtocol.selectedState(); }
    JSONObject vfoCapabilities() { return remoteProtocol.capabilities(); }

    void connect(String host, int port) {
        disconnect();
        if (host == null || host.trim().isEmpty() || port < 1 || port > 65535) {
            notifyStatus("Enter a server address and port between 1 and 65535");
            return;
        }
        synchronized (writeLock) {
            this.host = host;
            this.port = port;
            connectionRequested = true;
            retryCount = 0;
            startAttemptLocked();
        }
    }

    void disconnect() {
        synchronized (writeLock) {
            ++generation;
            connectionRequested = false;
            if (retry != null) retry.cancel(false);
            retry = null;
            closeTransportLocked();
            remoteProtocol.stopRestoring();
        }
    }

    void shutdown() {
        disconnect();
        executor.shutdownNow();
        writerExecutor.shutdownNow();
        retryExecutor.shutdownNow();
    }

    boolean sendControl(String action,
                        RadioSettings settings,
                        boolean serverDisableLocalVisualAudio,
                        boolean fullResolutionSpectrumFrames,
                        JSONObject extra) {
        final long token = generation;
        if (!isConnected()) {
            notifyStatus("Control channel is not ready");
            return false;
        }
        if (remoteVfoMode) {
            try {
                return queueJson(remoteProtocol.command(action, settings), token);
            } catch (JSONException | IllegalArgumentException | IllegalStateException e) {
                notifyStatus(e.getMessage());
                return false;
            }
        }
        if (!localClientHasControl && !NetworkControlProtocol.allowedForObserver(action)) {
            notifyStatus("This Android client is observer. Request control first.");
            return false;
        }
        if (NetworkControlProtocol.carriesSettings(action) && !controlProtocol.hasConfirmedState()) {
            requestServerState();
            notifyStatus("Waiting for current server settings; try again after synchronization");
            return false;
        }
        try {
            return queueJson(controlProtocol.command(action, settings,
                    serverDisableLocalVisualAudio, fullResolutionSpectrumFrames, extra), token);
        } catch (JSONException e) {
            notifyStatus("Could not build command: " + e.getMessage());
            return false;
        }
    }

    void requestServerState() {
        if (!isConnected()) return;
        try {
            queueJson(remoteVfoMode ? remoteProtocol.open() :
                    controlProtocol.command("requestServerState", null, false, false, null));
        } catch (JSONException e) {
            notifyStatus("Could not request server state: " + e.getMessage());
        }
    }

    private void startAttemptLocked() {
        retry = null;
        running = true;
        final long token = ++generation;
        executor.execute(() -> runConnection(host, port, token));
    }

    private void runConnection(String host, int port, long token) {
        Socket nextSocket = null;
        String failure = "Server closed connection";
        try {
            notifyStatus("Connecting to " + host + ":" + port);
            nextSocket = new Socket();
            nextSocket.setTcpNoDelay(true);
            nextSocket.setSoTimeout(15000);
            synchronized (writeLock) {
                if (token != generation || !connectionRequested) { nextSocket.close(); return; }
                socket = nextSocket;
            }
            nextSocket.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
            synchronized (writeLock) {
                if (token != generation || !connectionRequested) return;
                outputStream = nextSocket.getOutputStream();
            }
            sendProtocolLine(HELLO, token);
            notifyStatus("Connected, waiting for server handshake");
            readLoop(new BufferedInputStream(nextSocket.getInputStream(), 64 * 1024), token);
        } catch (IOException e) {
            failure = "Connection failed: " + e.getMessage();
        } finally {
            boolean notify = false;
            int delay = 0;
            synchronized (writeLock) {
                if (token == generation && connectionRequested) {
                    closeTransportLocked();
                    notify = true;
                    if (autoReconnect && !retryExecutor.isShutdown()) {
                        delay = (int) Math.min(15000L, (long) retryBaseMs << Math.min(retryCount++, 4));
                        retry = retryExecutor.schedule(() -> {
                            synchronized (writeLock) {
                                if (token == generation && connectionRequested) startAttemptLocked();
                            }
                        }, delay, TimeUnit.MILLISECONDS);
                    } else connectionRequested = false;
                }
            }
            if (nextSocket != null) try { nextSocket.close(); } catch (IOException ignored) { }
            if (notify) {
                listener.onDisconnected(failure);
                if (delay > 0) notifyStatus("Reconnecting in " + delay / 1000.0 + " s");
            }
        }
    }

    private void readLoop(InputStream inputStream, long token) throws IOException {
        while (running && token == generation) {
            String line = readLine(inputStream);
            if (token != generation || !running) return;
            if (line == null) {
                throw new EOFException("Server closed connection");
            }
            if (line.isEmpty()) {
                continue;
            }
            if (OK.equals(line)) {
                synchronized (writeLock) {
                    if (token != generation) return;
                    controlReady = true;
                    retryCount = 0;
                }
                requestServerState();
                if (remoteVfoMode) {
                    synchronized (writeLock) {
                        vfoDeadline = retryExecutor.schedule(() -> {
                            boolean unsupported = false;
                            synchronized (writeLock) {
                                if (token == generation && connectionRequested && !remoteProtocol.isReady()) {
                                    connectionRequested = false;
                                    closeTransportLocked();
                                    unsupported = true;
                                }
                            }
                            if (unsupported) listener.onDisconnected("Server did not provide a remote VFO session");
                        }, 8, TimeUnit.SECONDS);
                    }
                }
                notifyStatus("Control channel ready");
                continue;
            }
            if (PING.equals(line)) {
                sendProtocolLine(PONG, token);
                continue;
            }
            if (PONG.equals(line) || HELLO.equals(line)) {
                continue;
            }

            JSONObject command;
            try {
                command = new JSONObject(line);
            } catch (JSONException e) {
                notifyStatus("Ignoring invalid JSON: " + e.getMessage());
                continue;
            }

            if (command.has("payloadBytes")) {
                long bytes = parsePayloadBytes(command);
                if (bytes <= 0L || bytes > MAX_BINARY_BYTES) {
                    notifyStatus("Ignoring invalid binary payload header");
                    throw new IOException("Invalid binary payload header");
                }
                if ("spectrumbin".equals(command.optString("type"))) {
                    SpectrumFrameDecoder.validateBinaryHeader(command, bytes);
                    if (!remoteVfoMode || remoteProtocol.acceptsFrame(command)) {
                        byte[] payload = readFully(inputStream, bytes);
                        SpectrumFrame frame = parseBinarySpectrum(command, payload);
                        if (frame != null && token == generation) {
                            listener.onSpectrum(frame);
                        }
                    } else {
                        skipFully(inputStream, bytes);
                    }
                } else if ("remoteVfoAudio".equals(command.optString("type")) && remoteVfoMode) {
                    if (bytes > 1024 * 1024) throw new IOException("Remote audio frame too large");
                    byte[] pcm = readFully(inputStream, bytes);
                    if (token == generation && remoteProtocol.acceptsFrame(command) &&
                            command.optInt("sampleRate") == 48000 && command.optInt("channels") == 1 &&
                            "pcm_s16le".equals(command.optString("sampleFormat")) && (pcm.length & 1) == 0) {
                        if (command.optBoolean("reset")) listener.onAudioReset();
                        listener.onAudio(pcm);
                    }
                } else {
                    skipFully(inputStream, bytes);
                }
                continue;
            }

            handleJson(command, token);
        }
    }

    private void handleJson(JSONObject command, long token) {
        if (token != generation) return;
        String type = command.optString("type");
        if ("remoteVfo".equals(type) && remoteVfoMode) {
            try {
                for (JSONObject restore : remoteProtocol.acceptState(command)) queueJson(restore, token);
                if ("audioReset".equals(command.optString("action"))) listener.onAudioReset();
                listener.onControlMessage(command);
            } catch (JSONException e) { notifyStatus("Invalid VFO state: " + e.getMessage()); }
            return;
        }
        if ("spectrum".equals(type)) {
            if (remoteVfoMode && !remoteProtocol.acceptsFrame(command)) return;
            SpectrumFrame frame = parseSpectrum(command);
            if (frame != null) {
                listener.onSpectrum(frame);
            }
            return;
        }
        if ("audio".equals(type)) {
            if (remoteVfoMode) return;
            if (command.optInt("sampleRate", 48_000) != 48_000 ||
                    command.optInt("channels", 1) != 1 ||
                    !"pcm_s16le".equals(command.optString("sampleFormat", "pcm_s16le"))) {
                notifyStatus("Unsupported audio frame format");
                return;
            }
            String encoded = command.optString("pcm", "");
            if (!encoded.isEmpty()) {
                try {
                    listener.onAudio(Base64.decode(encoded, Base64.DEFAULT));
                } catch (IllegalArgumentException e) {
                    notifyStatus("Ignoring invalid audio payload");
                }
            }
            return;
        }
        if ("control".equals(type)) {
            if (remoteVfoMode) return;
            String action = command.optString("action");
            if (("serverState".equals(action) || "settingsAck".equals(action)) &&
                    !controlProtocol.acceptState(command)) return;
            if ("role".equals(action)) {
                localClientHasControl = command.optBoolean("canControl", localClientHasControl);
                listener.onRole(
                        localClientHasControl,
                        command.optString("peerId", ""),
                        command.optString("peerLabel", ""),
                        command.optString("controllerPeerId", ""));
            }
            listener.onControlMessage(command);
        }
    }

    private SpectrumFrame parseSpectrum(JSONObject command) {
        final int count;
        try {
            count = SpectrumFrameDecoder.validateJsonCount(command);
        } catch (IOException invalid) {
            notifyStatus(invalid.getMessage());
            return null;
        }
        JSONArray frequencyArray = command.optJSONArray("frequencies");
        JSONArray magnitudeArray = command.optJSONArray("magnitudes");
        boolean ascending = SpectrumFrameDecoder.isAscending(command);
        double[] frequencies = new double[count];
        float[] magnitudes = new float[count];
        for (int i = 0; i < count; ++i) {
            frequencies[i] = frequencyArray.optDouble(i, 0.0);
            float value = (float) magnitudeArray.optDouble(
                    SpectrumFrameDecoder.sourceIndex(i, count, ascending), -160.0);
            magnitudes[i] = Float.isFinite(value) ? value : -160.0f;
        }
        return new SpectrumFrame(
                frequencies,
                magnitudes,
                command.optDouble("minFrequency", frequencies[0]),
                command.optDouble("maxFrequency", frequencies[count - 1]),
                optionalDouble(command, "centerFrequency"),
                optionalDouble(command, "listeningFrequency"),
                command.optDouble("sampleRate", 0.0),
                command.optDouble("bandwidth", 0.0),
                command.optInt("modulationType", RadioSettings.MOD_AM),
                command.has("inputMode") ? command.optInt("inputMode", -1) : -1);
    }

    private SpectrumFrame parseBinarySpectrum(JSONObject command, byte[] payload) {
        final float[] magnitudes;
        try {
            magnitudes = SpectrumFrameDecoder.decodeBinary(command, payload);
        } catch (IOException invalid) {
            notifyStatus(invalid.getMessage());
            return null;
        }
        int count = magnitudes.length;

        double minFrequency = command.optDouble("minFrequency", 0.0);
        double maxFrequency = command.optDouble("maxFrequency", minFrequency + Math.max(1, count - 1));
        if (!Double.isFinite(minFrequency) || !Double.isFinite(maxFrequency) || maxFrequency <= minFrequency) {
            maxFrequency = minFrequency + Math.max(1, count - 1);
        }

        double[] frequencies = new double[count];
        for (int i = 0; i < count; ++i) {
            double fraction = count <= 1 ? 0.0 : i / (double) (count - 1);
            frequencies[i] = minFrequency + fraction * (maxFrequency - minFrequency);
        }

        return new SpectrumFrame(
                frequencies,
                magnitudes,
                minFrequency,
                maxFrequency,
                optionalDouble(command, "centerFrequency"),
                optionalDouble(command, "listeningFrequency"),
                command.optDouble("sampleRate", 0.0),
                command.optDouble("bandwidth", 0.0),
                command.optInt("modulationType", RadioSettings.MOD_AM),
                command.has("inputMode") ? command.optInt("inputMode", -1) : -1);
    }

    private static double optionalDouble(JSONObject command, String key) {
        return command.has(key) ? command.optDouble(key, Double.NaN) : Double.NaN;
    }

    private String readLine(InputStream inputStream) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream(512);
        while (running) {
            int value = inputStream.read();
            if (value < 0) {
                return null;
            }
            if (value == '\n') {
                break;
            }
            if (value != '\r') {
                line.write(value);
            }
            if (line.size() > MAX_LINE_BYTES) {
                throw new IOException("Protocol line is too large");
            }
        }
        return line.toString(StandardCharsets.UTF_8.name()).trim();
    }

    private void skipFully(InputStream inputStream, long bytes) throws IOException {
        long remaining = bytes;
        byte[] scratch = new byte[32 * 1024];
        while (running && remaining > 0L) {
            int read = inputStream.read(scratch, 0, (int) Math.min(scratch.length, remaining));
            if (read < 0) {
                throw new EOFException("Server closed connection during binary payload");
            }
            remaining -= read;
        }
    }

    private byte[] readFully(InputStream inputStream, long bytes) throws IOException {
        if (bytes > Integer.MAX_VALUE) {
            throw new IOException("Binary payload is too large");
        }
        byte[] payload = new byte[(int) bytes];
        int offset = 0;
        while (running && offset < payload.length) {
            int read = inputStream.read(payload, offset, payload.length - offset);
            if (read < 0) {
                throw new EOFException("Server closed connection during binary payload");
            }
            offset += read;
        }
        return payload;
    }

    private long parsePayloadBytes(JSONObject command) {
        Object value = command.opt("payloadBytes");
        if (value instanceof Number) {
            return ((Number) value).longValue();
        }
        if (value instanceof String) {
            try {
                return Long.parseLong((String) value);
            } catch (NumberFormatException ignored) {
                return -1L;
            }
        }
        return -1L;
    }

    private boolean queueJson(JSONObject command) {
        return queueJson(command, generation);
    }

    private boolean queueJson(JSONObject command, long token) {
        if (token != generation || !controlReady) return false;
        final String line = command.toString();
        try {
            writerExecutor.execute(() -> sendProtocolLine(line, token));
            return true;
        } catch (RejectedExecutionException e) {
            notifyStatus("Network writer is not available");
            return false;
        }
    }

    private boolean sendProtocolLine(String line, long token) {
        synchronized (writeLock) {
            if (outputStream == null || token != generation) {
                return false;
            }
            try {
                outputStream.write((line + "\n").getBytes(StandardCharsets.UTF_8));
                outputStream.flush();
                return true;
            } catch (IOException e) {
                // Wake the reader; its finally block owns retry and notification.
                try { if (socket != null) socket.close(); } catch (IOException ignored) { }
                return false;
            }
        }
    }

    private void closeTransportLocked() {
        if (vfoDeadline != null) vfoDeadline.cancel(false);
        vfoDeadline = null;
        running = false;
        controlReady = false;
        localClientHasControl = false;
        controlProtocol.reset();
        remoteProtocol.resetConnection();
        if (socket != null) try { socket.close(); } catch (IOException ignored) { }
        socket = null;
        outputStream = null;
    }

    private void notifyStatus(String status) {
        listener.onStatus(status);
    }

}
