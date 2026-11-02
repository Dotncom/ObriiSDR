package com.fobosapp.networkclient;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import java.util.List;
import static org.junit.Assert.*;

public class RemoteVfoProtocolTest {
    private JSONObject session(String id, JSONArray vfos) throws Exception {
        return new JSONObject().put("type", "remoteVfo").put("action", "session")
                .put("sessionId", id).put("capabilities", new JSONObject()
                        .put("centerFrequency", 100e6).put("sampleRate", 16e6)
                        .put("receiverRunning", true)).put("vfos", vfos);
    }
    private RadioSettings radio(double frequency) {
        RadioSettings value = new RadioSettings();
        value.listeningFrequency = frequency;
        value.modulationType = RadioSettings.MOD_NFM;
        value.bandwidth = 12500;
        value.fftLength = 2048;
        return value;
    }
    @Test public void vfoCannotChangeHardwareOrSharedReceiver() throws Exception {
        RemoteVfoProtocol protocol = new RemoteVfoProtocol();
        protocol.acceptState(session("one", new JSONArray()));
        JSONObject command = protocol.command("start", radio(100.1e6));
        assertEquals("remoteVfo", command.getString("type"));
        assertEquals("activate", command.getString("action"));
        JSONObject values = command.getJSONObject("settings");
        assertFalse(values.has("deviceIndex"));
        assertFalse(values.has("sampleRate"));
        assertFalse(values.has("centerFrequency"));
        assertFalse(command.has("processingMode"));
        try { protocol.command("setProcessingMode", radio(100e6)); fail(); }
        catch (IllegalArgumentException expected) { }
    }
    @Test public void reconnectRestoresBothOwnVfosOnceWithTheirOwnFrequencies() throws Exception {
        RemoteVfoProtocol protocol = new RemoteVfoProtocol();
        protocol.acceptState(session("old", new JSONArray()));
        protocol.command("start", radio(100.1e6));
        JSONObject a = new JSONObject().put("vfoId", "A").put("active", true)
                .put("frequencyHz", 100.1e6);
        protocol.acceptState(session("old", new JSONArray().put(a)));
        protocol.select("B");
        protocol.command("start", radio(100.2e6));
        JSONObject b = new JSONObject().put("vfoId", "B").put("active", true)
                .put("frequencyHz", 100.2e6);
        protocol.acceptState(session("old", new JSONArray().put(a).put(b)));
        protocol.resetConnection();
        List<JSONObject> restore = protocol.acceptState(session("new", new JSONArray()));
        assertEquals(2, restore.size());
        assertEquals("A", restore.get(0).getString("vfoId"));
        assertEquals(100.1e6, restore.get(0).getJSONObject("settings").getDouble("frequencyHz"), 0.01);
        assertEquals("B", restore.get(1).getString("vfoId"));
        assertEquals(100.2e6, restore.get(1).getJSONObject("settings").getDouble("frequencyHz"), 0.01);
        assertTrue(protocol.acceptState(session("new", new JSONArray())).isEmpty());
        assertFalse(protocol.acceptsFrame(new JSONObject().put("vfoId", "A").put("sessionId", "new")));
        assertFalse(protocol.acceptsFrame(new JSONObject().put("vfoId", "B").put("sessionId", "old")));
        assertTrue(protocol.acceptsFrame(new JSONObject().put("vfoId", "B").put("sessionId", "new")));
    }
    @Test public void manualDisconnectAndStopPreventAutomaticActivation() throws Exception {
        RemoteVfoProtocol protocol = new RemoteVfoProtocol();
        protocol.acceptState(session("one", new JSONArray()));
        protocol.command("start", radio(100e6));
        protocol.stopRestoring();
        protocol.resetConnection();
        assertTrue(protocol.acceptState(session("two", new JSONArray())).isEmpty());
    }
    @Test public void rejectedActivationIsNotRepeatedAfterReconnect() throws Exception {
        RemoteVfoProtocol protocol = new RemoteVfoProtocol();
        protocol.acceptState(session("one", new JSONArray()));
        JSONObject request = protocol.command("start", radio(900e6));
        protocol.acceptState(new JSONObject().put("action", "error")
                .put("requestId", request.getString("requestId")));
        protocol.resetConnection();
        assertTrue(protocol.acceptState(session("two", new JSONArray())).isEmpty());
    }
}
