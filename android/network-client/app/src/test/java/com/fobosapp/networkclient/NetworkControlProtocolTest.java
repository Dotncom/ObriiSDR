package com.fobosapp.networkclient;

import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class NetworkControlProtocolTest {
    @Test public void settingsPreservePhysicalReceiverAndServerPolicies() throws Exception {
        NetworkControlProtocol protocol = new NetworkControlProtocol();
        RadioSettings settings = new RadioSettings();
        settings.deviceIndex = 0;
        JSONObject message = protocol.command("settings", settings, true, true, null);
        assertFalse(message.getJSONObject("settings").has("deviceIndex"));
        assertFalse(message.has("processingMode"));
        assertFalse(message.has("serverDisableLocalVisualAudio"));
        assertTrue(message.getBoolean("fullResolutionSpectrumFrames"));
        assertFalse(message.getString("requestId").isEmpty());
    }

    @Test public void observerCanSynchronizeWithoutChangingSettings() throws Exception {
        assertTrue(NetworkControlProtocol.allowedForObserver("requestServerState"));
        assertFalse(NetworkControlProtocol.allowedForObserver("start"));
        assertFalse(NetworkControlProtocol.allowedForObserver("setServerLocalOutput"));
        JSONObject request = new NetworkControlProtocol().command(
                "requestServerState", null, true, true, null);
        assertEquals(2, request.length());
    }

    @Test public void stopAndPriorityDoNotCarryReceiverSettings() throws Exception {
        for (String action : new String[]{"stop", "requestPriority", "priorityResponse"}) {
            JSONObject message = new NetworkControlProtocol().command(action, null, true, true, null);
            assertFalse(message.has("settings"));
            assertFalse(message.has("processingMode"));
            assertFalse(message.has("serverDisableLocalVisualAudio"));
        }
    }

    @Test public void localOutputUsesDedicatedCommand() throws Exception {
        JSONObject message = new NetworkControlProtocol().command(
                "setServerLocalOutput", null, true, false, null);
        assertTrue(message.getBoolean("serverDisableLocalVisualAudio"));
        assertFalse(message.has("settings"));
    }

    @Test public void staleAcknowledgementCannotReplaceNewerTuning() throws Exception {
        NetworkControlProtocol protocol = new NetworkControlProtocol();
        JSONObject first = protocol.command("settings", new RadioSettings(), false, false, null);
        JSONObject second = protocol.command("settings", new RadioSettings(), false, false, null);
        JSONObject ack = new JSONObject().put("action", "settingsAck")
                .put("settings", new JSONObject()).put("requestId", first.getString("requestId"));
        assertFalse(protocol.acceptState(ack));
        assertFalse(protocol.acceptState(new JSONObject().put("action", "serverState")
                .put("settings", new JSONObject())));
        ack.put("requestId", second.getString("requestId")).put("ok", false);
        assertTrue(protocol.acceptState(ack));
        assertTrue(protocol.hasConfirmedState());
        protocol.reset();
        assertFalse(protocol.hasConfirmedState());
    }

    @Test public void stateRecoveryClearsMissingAcknowledgement() throws Exception {
        NetworkControlProtocol protocol = new NetworkControlProtocol();
        protocol.command("start", new RadioSettings(), false, false, null);
        protocol.command("requestServerState", null, false, false, null);
        assertTrue(protocol.acceptState(new JSONObject().put("action", "serverState")
                .put("settings", new JSONObject().put("deviceIndex", 4000))));
    }
}
