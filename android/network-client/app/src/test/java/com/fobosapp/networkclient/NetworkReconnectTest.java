package com.fobosapp.networkclient;

import org.json.JSONObject;
import org.junit.Test;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.*;
import static org.junit.Assert.*;

public class NetworkReconnectTest {
    private static class Events implements FobosNetworkClient.Listener {
        final BlockingQueue<JSONObject> controls = new LinkedBlockingQueue<>();
        final BlockingQueue<byte[]> audio = new LinkedBlockingQueue<>();
        public void onStatus(String message) { }
        public void onDisconnected(String message) { }
        public void onRole(boolean control, String id, String label, String owner) { }
        public void onSpectrum(FobosNetworkClient.SpectrumFrame frame) { }
        public void onAudio(byte[] pcm) { audio.add(pcm); }
        public void onControlMessage(JSONObject message) { controls.add(message); }
    }
    private BufferedReader reader(Socket socket) throws Exception {
        socket.setSoTimeout(4000);
        return new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
    }
    private void send(Socket socket, String lines) throws Exception {
        socket.getOutputStream().write(lines.getBytes(StandardCharsets.UTF_8));
        socket.getOutputStream().flush();
    }
    @Test(timeout=15000) public void reconnectSynchronizesStateWithoutReplayingSharedStart() throws Exception {
        Events events = new Events();
        FobosNetworkClient client = new FobosNetworkClient(events, 50);
        try (ServerSocket server = new ServerSocket(0, 2, InetAddress.getLoopbackAddress())) {
            server.setSoTimeout(5000);
            client.connect(server.getInetAddress().getHostAddress(), server.getLocalPort());
            try (Socket first = server.accept()) {
                BufferedReader input = reader(first);
                assertEquals("FOBOSAPP_HELLO 1", input.readLine());
                send(first, "FOBOSAPP_OK 1\n{\"type\":\"control\",\"action\":\"role\",\"canControl\":true}\n" +
                        "{\"type\":\"control\",\"action\":\"serverState\",\"settings\":{}}\n");
                assertEquals("requestServerState", new JSONObject(input.readLine()).getString("action"));
                assertNotNull(events.controls.poll(3, TimeUnit.SECONDS));
                assertNotNull(events.controls.poll(3, TimeUnit.SECONDS));
                assertTrue(client.sendControl("start", new RadioSettings(), false, false, null));
                assertEquals("start", new JSONObject(input.readLine()).getString("action"));
            }
            try (Socket second = server.accept()) {
                BufferedReader input = reader(second);
                assertEquals("FOBOSAPP_HELLO 1", input.readLine());
                assertFalse(client.canControl());
                send(second, "FOBOSAPP_OK 1\n{\"type\":\"control\",\"action\":\"role\",\"canControl\":false}\n");
                JSONObject request = new JSONObject(input.readLine());
                assertEquals("requestServerState", request.getString("action"));
                assertFalse(request.has("settings"));
                assertNotNull(events.controls.poll(3, TimeUnit.SECONDS));
                assertFalse(client.canControl());
                second.setSoTimeout(200);
                try { input.readLine(); fail("Must not replay start or request control after reconnect"); }
                catch (SocketTimeoutException expected) { }
                client.disconnect();
                assertFalse(client.isActive());
            }
            server.setSoTimeout(250);
            try (Socket unexpected = server.accept()) { fail("Manual disconnect must cancel retries"); }
            catch (SocketTimeoutException expected) { }
        } finally { client.shutdown(); }
    }
    @Test(timeout=12000) public void cancelledAttemptCannotCloseNewConnection() throws Exception {
        Events events = new Events();
        FobosNetworkClient client = new FobosNetworkClient(events, 50);
        try (ServerSocket server = new ServerSocket(0, 2, InetAddress.getLoopbackAddress())) {
            server.setSoTimeout(4000);
            String host = server.getInetAddress().getHostAddress();
            client.connect(host, server.getLocalPort());
            try (Socket old = server.accept()) {
                assertEquals("FOBOSAPP_HELLO 1", reader(old).readLine());
                client.disconnect();
                client.connect(host, server.getLocalPort());
                try (Socket fresh = server.accept()) {
                    BufferedReader input = reader(fresh);
                    assertEquals("FOBOSAPP_HELLO 1", input.readLine());
                    send(fresh, "FOBOSAPP_OK 1\n{\"type\":\"control\",\"action\":\"role\",\"canControl\":false}\n");
                    assertEquals("requestServerState", new JSONObject(input.readLine()).getString("action"));
                    assertNotNull(events.controls.poll(3, TimeUnit.SECONDS));
                    assertTrue(client.isConnected());
                }
            }
        } finally { client.shutdown(); }
    }

    private String vfoState(String session, boolean active) {
        return "{\"type\":\"remoteVfo\",\"action\":\"session\",\"sessionId\":\"" + session +
                "\",\"capabilities\":{\"centerFrequency\":100000000,\"sampleRate\":2000000," +
                "\"receiverRunning\":true},\"vfos\":" + (active ?
                "[{\"vfoId\":\"A\",\"active\":true,\"frequencyHz\":100000000,\"bandwidthHz\":12500}]" : "[]") + "}\n";
    }
    @Test(timeout=15000) public void remoteReconnectRestoresOwnVfoAndFiltersBinaryAudio() throws Exception {
        Events events = new Events();
        FobosNetworkClient client = new FobosNetworkClient(events, 50);
        client.setRemoteVfoMode(true);
        try (ServerSocket server = new ServerSocket(0, 2, InetAddress.getLoopbackAddress())) {
            server.setSoTimeout(4000);
            client.connect(server.getInetAddress().getHostAddress(), server.getLocalPort());
            try (Socket first = server.accept()) {
                BufferedReader input = reader(first);
                assertEquals("FOBOSAPP_HELLO 1", input.readLine());
                send(first, "FOBOSAPP_OK 1\n");
                JSONObject open = new JSONObject(input.readLine());
                assertEquals("remoteVfo", open.getString("type"));
                assertEquals("open", open.getString("action"));
                send(first, vfoState("first", false));
                assertNotNull(events.controls.poll(3, TimeUnit.SECONDS));
                RadioSettings radio = new RadioSettings();
                radio.modulationType = RadioSettings.MOD_NFM;
                assertTrue(client.canControl()); // Independent listener needs no hardware-control role.
                assertTrue(client.sendControl("start", radio, false, false, null));
                assertEquals("activate", new JSONObject(input.readLine()).getString("action"));
                send(first, vfoState("first", true));
                assertNotNull(events.controls.poll(3, TimeUnit.SECONDS));
                for (String id : new String[]{"B", "A"}) {
                    send(first, "{\"type\":\"remoteVfoAudio\",\"vfoId\":\"" + id +
                            "\",\"sessionId\":\"first\",\"sampleRate\":48000,\"channels\":1," +
                            "\"sampleFormat\":\"pcm_s16le\",\"payloadBytes\":4}\n");
                    first.getOutputStream().write(new byte[]{1, 0, 2, 0});
                    first.getOutputStream().flush();
                }
                assertArrayEquals(new byte[]{1, 0, 2, 0}, events.audio.poll(3, TimeUnit.SECONDS));
                assertNull(events.audio.poll(100, TimeUnit.MILLISECONDS));
            }
            try (Socket second = server.accept()) {
                BufferedReader input = reader(second);
                assertEquals("FOBOSAPP_HELLO 1", input.readLine());
                send(second, "FOBOSAPP_OK 1\n");
                assertEquals("open", new JSONObject(input.readLine()).getString("action"));
                send(second, vfoState("second", false));
                JSONObject restore = new JSONObject(input.readLine());
                assertEquals("remoteVfo", restore.getString("type"));
                assertEquals("activate", restore.getString("action"));
                assertEquals("A", restore.getString("vfoId"));
                assertFalse(restore.getJSONObject("settings").has("deviceIndex"));
                client.disconnect();
            }
        } finally { client.shutdown(); }
    }
}
