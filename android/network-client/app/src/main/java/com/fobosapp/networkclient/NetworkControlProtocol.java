package com.fobosapp.networkclient;

import org.json.JSONException;
import org.json.JSONObject;

/** Control-channel contract shared with main_network.cpp. No Android dependencies. */
final class NetworkControlProtocol {
    private long requestCounter;
    private String pendingRequestId = "";
    private boolean confirmedState;

    synchronized void reset() {
        pendingRequestId = "";
        confirmedState = false;
    }

    synchronized boolean hasConfirmedState() {
        return confirmedState;
    }

    static boolean carriesSettings(String action) {
        return "settings".equals(action) || "setParameters".equals(action) || "start".equals(action);
    }

    static boolean allowedForObserver(String action) {
        return "requestServerState".equals(action) || "requestPriority".equals(action) ||
                "priorityResponse".equals(action);
    }

    synchronized JSONObject command(String action, RadioSettings settings,
                                    boolean disableLocalOutput, boolean fullResolution,
                                    JSONObject extra) throws JSONException {
        JSONObject result = new JSONObject();
        result.put("type", "control");
        result.put("action", action);
        if (carriesSettings(action)) {
            JSONObject values = settings.toJson();
            // Android has no server-receiver selector. Preserve the host's selection.
            values.remove("deviceIndex");
            pendingRequestId = Long.toString(++requestCounter);
            result.put("requestId", pendingRequestId);
            result.put("settings", values);
            result.put("fullResolutionSpectrumFrames", fullResolution);
        } else if ("requestServerState".equals(action)) {
            pendingRequestId = "";
        } else if ("setServerLocalOutput".equals(action)) {
            result.put("serverDisableLocalVisualAudio", disableLocalOutput);
        }
        if (extra != null) {
            java.util.Iterator<String> keys = extra.keys();
            while (keys.hasNext()) {
                String key = keys.next();
                result.put(key, extra.get(key));
            }
        }
        return result;
    }

    synchronized boolean acceptState(JSONObject message) {
        String action = message.optString("action");
        if (!"serverState".equals(action) && !"settingsAck".equals(action)) return false;
        if (message.optJSONObject("settings") == null) return false;
        if ("settingsAck".equals(action)) {
            String id = message.optString("requestId");
            if (!id.isEmpty() && !id.equals(pendingRequestId)) return false;
        } else if (!pendingRequestId.isEmpty()) {
            return false;
        }
        pendingRequestId = "";
        confirmedState = true;
        return true;
    }
}
