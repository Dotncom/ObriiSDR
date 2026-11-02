package com.fobosapp.networkclient;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

import static org.junit.Assert.*;

public class SpectrumFrameDecoderTest {
    private static JSONObject header(String format, int count, boolean reference) throws Exception {
        return new JSONObject().put("type", "spectrumbin").put("binFormat", format)
                .put("fftLength", count).put("hasReferenceMagnitudes", reference);
    }

    private static byte[] floats(float... values) {
        ByteBuffer buffer = ByteBuffer.allocate(values.length * Float.BYTES).order(ByteOrder.LITTLE_ENDIAN);
        for (float value : values) buffer.putFloat(value);
        return buffer.array();
    }

    private static byte[] shorts(short... values) {
        ByteBuffer buffer = ByteBuffer.allocate(values.length * Short.BYTES).order(ByteOrder.LITTLE_ENDIAN);
        for (short value : values) buffer.putShort(value);
        return buffer.array();
    }

    @Test public void f32LittleEndianAndNonfiniteComponentsUseDisplayFloor() throws Exception {
        JSONObject frame = header("f32le", 5, false);
        byte[] payload = floats(-12.5f, Float.NaN, Float.POSITIVE_INFINITY, -80.0f, -0.25f);
        assertArrayEquals(new float[]{-80.0f, -0.25f, -12.5f, -160.0f, -160.0f},
                SpectrumFrameDecoder.decodeBinary(frame, payload), 0.0f);
        frame.remove("binFormat");
        assertEquals(5, SpectrumFrameDecoder.validateBinaryHeader(frame, payload.length));
    }

    @Test public void signedDb10SupportsFullRangeAndSkipsExactlyOneReferencePlane() throws Exception {
        JSONObject frame = header("s16le-db10", 7, true).put("magnitudeOrder", "ascending-frequency");
        byte[] payload = shorts(Short.MIN_VALUE, (short) -1600, (short) -123, (short) -1,
                (short) 0, (short) 1, Short.MAX_VALUE,
                (short) 10, (short) 11, (short) 12, (short) 13, (short) 14, (short) 15, (short) 16);
        assertArrayEquals(new float[]{-3276.8f, -160.0f, -12.3f, -0.1f, 0.0f, 0.1f, 3276.7f},
                SpectrumFrameDecoder.decodeBinary(frame, payload), 0.0001f);
        for (int bytes : new int[]{0, 13, 14, 26, 27, 29, 30, 56}) {
            assertThrows(IOException.class, () -> SpectrumFrameDecoder.decodeBinary(frame, new byte[bytes]));
        }
        frame.put("hasReferenceMagnitudes", false);
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.decodeBinary(frame, payload));
        assertEquals(7, SpectrumFrameDecoder.validateBinaryHeader(frame, 14));
    }

    @Test public void fftNativeOrderingUsesCeilingForOddAndPreservesExplicitAscending() throws Exception {
        for (int count : new int[]{1, 2, 3, 5, 6, 7, 8, 9, 513}) {
            float[] raw = new float[count];
            short[] compact = new short[count];
            float[] shifted = new float[count];
            for (int i = 0; i < count; ++i) {
                raw[i] = i;
                compact[i] = (short) (10 * i);
                shifted[i] = (i + (count + 1) / 2) % count;
            }
            for (String format : new String[]{"f32le", "s16le-db10"}) {
                JSONObject frame = header(format, count, false);
                byte[] payload = "f32le".equals(format) ? floats(raw) : shorts(compact);
                assertArrayEquals(shifted, SpectrumFrameDecoder.decodeBinary(frame, payload), 0.0f);
                frame.put("magnitudeOrder", "fft-native");
                assertArrayEquals(shifted, SpectrumFrameDecoder.decodeBinary(frame, payload), 0.0f);
                frame.put("magnitudeOrder", "legacy-or-unknown");
                assertArrayEquals(shifted, SpectrumFrameDecoder.decodeBinary(frame, payload), 0.0f);
                frame.put("magnitudeOrder", "ascending-frequency");
                assertArrayEquals(raw, SpectrumFrameDecoder.decodeBinary(frame, payload), 0.0f);
            }
        }
    }

    @Test public void inferredLegacyCountRequiresWholePlanesForBothFormats() throws Exception {
        for (String format : new String[]{"f32le", "s16le-db10"}) {
            for (boolean reference : new boolean[]{false, true}) {
                JSONObject frame = header(format, 5, reference);
                frame.remove("fftLength");
                int stride = ("f32le".equals(format) ? 4 : 2) * (reference ? 2 : 1);
                assertEquals(5, SpectrumFrameDecoder.validateBinaryHeader(frame, 5 * stride));
                assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, 5 * stride - 1));
            }
        }
    }

    @Test public void headerBoundsAndExactByteSizesAreCheckedWithoutAllocatingPayloads() throws Exception {
        int max = SpectrumFrameDecoder.MAX_DISPLAY_BINS;
        for (String format : new String[]{"f32le", "s16le-db10"}) {
            for (boolean reference : new boolean[]{false, true}) {
                JSONObject frame = header(format, max, reference);
                long stride = ("f32le".equals(format) ? 4L : 2L) * (reference ? 2 : 1);
                assertEquals(max, SpectrumFrameDecoder.validateBinaryHeader(frame, max * stride));
                frame.put("fftLength", max + 1);
                assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, (max + 1L) * stride));
            }
        }
        JSONObject frame = header("f32le", 5, false);
        for (long bytes : new long[]{-1, 0, 1, 19, 21, 40, 512L * 1024 * 1024, Long.MAX_VALUE})
            assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, bytes));
        for (Object count : new Object[]{0, -1, 5.5, "invalid", JSONObject.NULL,
                "2147483648", "9223372036854775807", Integer.MAX_VALUE, 536870912}) {
            frame.put("fftLength", count);
            assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, 20));
        }
        frame.put("fftLength", "5").put("payloadBytes", "20");
        assertEquals(5, SpectrumFrameDecoder.validateBinaryHeader(frame, 20));
        frame.put("payloadBytes", 20.5);
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, 20));
        frame.put("payloadBytes", 24);
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, 20));
    }

    @Test public void malformedFormatsFlagsAndCountsNeverFallBackToAnAllocation() throws Exception {
        for (Object format : new Object[]{"", "f64le", "s16be-db10", JSONObject.NULL, 4}) {
            JSONObject frame = header("f32le", 5, false).put("binFormat", format);
            assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, 20));
        }
        for (Object reference : new Object[]{"true", "false", 0, 1, JSONObject.NULL}) {
            JSONObject frame = header("f32le", 5, false).put("hasReferenceMagnitudes", reference);
            assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(frame, 20));
        }
        JSONObject highPrecision = header("f32le", 5, false)
                .put("fftLength", new java.math.BigDecimal("5.00000000000000000001"));
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateBinaryHeader(highPrecision, 20));
    }

    @Test public void jsonDimensionsAreBoundedBeforeReadingAnyBins() throws Exception {
        JSONArray huge = new JSONArray() {
            @Override public int length() { return SpectrumFrameDecoder.MAX_DISPLAY_BINS + 1; }
        };
        JSONObject frame = new JSONObject().put("frequencies", huge).put("magnitudes", huge);
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateJsonCount(frame));
        frame.put("frequencies", new JSONArray("[1,2,3]")).put("magnitudes", new JSONArray("[1,2]"));
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateJsonCount(frame));
        frame.put("magnitudes", new JSONArray("[1,2,3]"));
        assertEquals(3, SpectrumFrameDecoder.validateJsonCount(frame));
        frame.remove("magnitudes");
        assertThrows(IOException.class, () -> SpectrumFrameDecoder.validateJsonCount(frame));
    }

    private static final class Events implements FobosNetworkClient.Listener {
        final BlockingQueue<FobosNetworkClient.SpectrumFrame> frames = new LinkedBlockingQueue<>();
        final BlockingQueue<String> disconnected = new LinkedBlockingQueue<>();
        public void onStatus(String message) { }
        public void onDisconnected(String reason) { disconnected.add(reason); }
        public void onRole(boolean control, String id, String label, String owner) { }
        public void onSpectrum(FobosNetworkClient.SpectrumFrame frame) { frames.add(frame); }
        public void onAudio(byte[] pcm) { }
        public void onControlMessage(JSONObject command) { }
    }

    private static BufferedReader handshake(Socket peer) throws Exception {
        peer.setSoTimeout(3000);
        BufferedReader input = new BufferedReader(new InputStreamReader(peer.getInputStream(), StandardCharsets.UTF_8));
        assertEquals("FOBOSAPP_HELLO 1", input.readLine());
        peer.getOutputStream().write("FOBOSAPP_OK 1\n".getBytes(StandardCharsets.UTF_8));
        peer.getOutputStream().flush();
        JSONObject request = new JSONObject(input.readLine());
        assertEquals("requestServerState", request.getString("action"));
        assertFalse(request.has("settings"));
        return input;
    }

    private static void packet(Socket peer, JSONObject header, byte[] payload) throws Exception {
        header.put("payloadBytes", payload.length);
        peer.getOutputStream().write((header.toString() + "\n").getBytes(StandardCharsets.UTF_8));
        peer.getOutputStream().write(payload);
        peer.getOutputStream().flush();
    }

    @Test(timeout = 10000) public void transportDecodesBothBinaryFormatsAndJsonWithoutReferenceDesync() throws Exception {
        Events events = new Events();
        FobosNetworkClient client = new FobosNetworkClient(events);
        client.setAutoReconnect(false);
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            server.setSoTimeout(3000);
            client.connect(server.getInetAddress().getHostAddress(), server.getLocalPort());
            try (Socket peer = server.accept()) {
                handshake(peer);
                // Unknown raw IQ still streams through the bounded skip buffer.
                packet(peer, new JSONObject().put("type", "iqbin"), new byte[]{0, 10, 13, 0, 10});
                packet(peer, header("s16le-db10", 5, true).put("minFrequency", 100).put("maxFrequency", 104),
                        shorts((short) -100, (short) -200, (short) -300, (short) -400, (short) -500,
                                (short) 10, (short) 11, (short) 12, (short) 13, (short) 14));
                packet(peer, header("f32le", 3, true).put("minFrequency", 200).put("maxFrequency", 202)
                                .put("magnitudeOrder", "ascending-frequency"),
                        floats(-1.5f, -2.5f, -3.5f, 10, 11, 12));
                for (String order : new String[]{"fft-native", "ascending-frequency"}) {
                    JSONObject json = new JSONObject().put("type", "spectrum").put("magnitudeOrder", order)
                            .put("frequencies", new JSONArray("[300,301,302,303,304]"))
                            .put("magnitudes", new JSONArray("[-10,-20,-30,-40,-50]"));
                    peer.getOutputStream().write((json.toString() + "\n").getBytes(StandardCharsets.UTF_8));
                }
                peer.getOutputStream().flush();
                float[][] expected = {{-40, -50, -10, -20, -30}, {-1.5f, -2.5f, -3.5f},
                        {-40, -50, -10, -20, -30}, {-10, -20, -30, -40, -50}};
                for (int index = 0; index < expected.length; ++index) {
                    FobosNetworkClient.SpectrumFrame frame = events.frames.poll(2, TimeUnit.SECONDS);
                    assertNotNull(frame);
                    assertArrayEquals(expected[index], frame.magnitudes, 0.0f);
                    assertEquals(index == 0 ? 100.0 : index == 1 ? 200.0 : 300.0, frame.frequencies[0], 0.0);
                    assertEquals(frame.frequencies.length, frame.magnitudes.length);
                }
                assertTrue(client.isConnected());
            }
        } finally { client.shutdown(); }
    }

    @Test(timeout = 7000) public void hugeDisplayHeaderIsRejectedWithoutReadingItsBody() throws Exception {
        Events events = new Events();
        FobosNetworkClient client = new FobosNetworkClient(events);
        client.setAutoReconnect(false);
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            server.setSoTimeout(3000);
            client.connect(server.getInetAddress().getHostAddress(), server.getLocalPort());
            try (Socket peer = server.accept()) {
                handshake(peer);
                JSONObject bad = header("f32le", 5, false).put("payloadBytes", 512L * 1024 * 1024);
                peer.getOutputStream().write((bad.toString() + "\n").getBytes(StandardCharsets.UTF_8));
                peer.getOutputStream().flush();
                String failure = events.disconnected.poll(2, TimeUnit.SECONDS);
                assertNotNull("Invalid display header must fail before waiting for any payload bytes", failure);
                assertTrue(failure, failure.contains("Spectrum"));
                assertNull(events.frames.poll());
                assertEquals(-1, peer.getInputStream().read());
            }
        } finally { client.shutdown(); }
    }
}
