package com.fobosapp.networkclient;

import org.json.JSONObject;
import org.junit.Test;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import static org.junit.Assert.*;

public class FobosNetworkClientTest {
    @Test(timeout = 12000) public void handshakeSynchronizesObserverWithoutRetuningServer() throws Exception {
        BlockingQueue<JSONObject> states = new LinkedBlockingQueue<>();
        FobosNetworkClient client = new FobosNetworkClient(new FobosNetworkClient.Listener() {
            public void onStatus(String text) { }
            public void onDisconnected(String reason) { }
            public void onRole(boolean control, String id, String label, String controller) { }
            public void onSpectrum(FobosNetworkClient.SpectrumFrame frame) { }
            public void onAudio(byte[] pcm) { }
            public void onControlMessage(JSONObject message) { states.add(message); }
        });
        try (ServerSocket server = new ServerSocket(0, 1, java.net.InetAddress.getLoopbackAddress())) {
            server.setSoTimeout(4000);
            client.connect(server.getInetAddress().getHostAddress(), server.getLocalPort());
            try (Socket peer = server.accept()) {
                peer.setSoTimeout(4000);
                BufferedReader input = new BufferedReader(new InputStreamReader(
                        peer.getInputStream(), StandardCharsets.UTF_8));
                OutputStream output = peer.getOutputStream();
                assertEquals("FOBOSAPP_HELLO 1", input.readLine());
                output.write(("FOBOSAPP_OK 1\n" +
                        "{\"type\":\"control\",\"action\":\"role\",\"canControl\":false}\n" +
                        "{\"type\":\"control\",\"action\":\"serverState\",\"settings\":{\"deviceIndex\":4000}}\n")
                        .getBytes(StandardCharsets.UTF_8));
                output.flush();
                JSONObject request = new JSONObject(input.readLine());
                assertEquals("requestServerState", request.getString("action"));
                assertFalse(request.has("settings"));
                assertNotNull(states.poll(4, TimeUnit.SECONDS)); // role
                JSONObject state = states.poll(4, TimeUnit.SECONDS);
                assertNotNull(state);
                assertEquals(4000, state.getJSONObject("settings").getInt("deviceIndex"));
                assertFalse(client.canControl());
                assertFalse(client.sendControl("settings", new RadioSettings(), true, true, null));
                assertTrue(client.sendControl("requestServerState", null, true, true, null));
                request = new JSONObject(input.readLine());
                assertEquals("requestServerState", request.getString("action"));
                assertEquals(2, request.length());
            }
        } finally {
            client.shutdown();
        }
    }
}
