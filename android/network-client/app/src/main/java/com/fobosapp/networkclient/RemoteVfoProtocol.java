package com.fobosapp.networkclient;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Two independent server DDC channels, with no physical receiver commands. */
final class RemoteVfoProtocol {
    private final Map<String, JSONObject> desired = new LinkedHashMap<>();
    private final Map<String, JSONObject> confirmed = new LinkedHashMap<>();
    private final Map<String, Boolean> resume = new LinkedHashMap<>();
    private final Map<String, String> requests = new LinkedHashMap<>();
    private String selected = "A";
    private String session = "";
    private JSONObject capabilities = new JSONObject();
    private long counter;
    private boolean ready;

    synchronized void select(String id) {
        if (!"A".equals(id) && !"B".equals(id)) throw new IllegalArgumentException("Unknown VFO");
        selected = id;
    }
    synchronized boolean isReady() { return ready; }
    synchronized void resetConnection() {
        ready = false;
        session = "";
        confirmed.clear();
        requests.clear();
    }
    synchronized void stopRestoring() { resume.clear(); }
    synchronized JSONObject capabilities() { return copy(capabilities); }
    synchronized JSONObject selectedState() {
        JSONObject state = copy(confirmed.containsKey(selected) ? confirmed.get(selected) : desired.get(selected));
        try {
            state.put("vfoId", selected);
            state.put("active", confirmed.containsKey(selected) && confirmed.get(selected).optBoolean("active"));
        } catch (JSONException impossible) { throw new IllegalStateException(impossible); }
        return state;
    }
    synchronized JSONObject open() throws JSONException {
        return base("open", selected);
    }
    synchronized JSONObject command(String action, RadioSettings settings) throws JSONException {
        if ("requestServerState".equals(action)) return open();
        if (!ready) throw new IllegalStateException("Waiting for remote VFO session");
        String remoteAction;
        if ("start".equals(action)) remoteAction = "activate";
        else if ("stop".equals(action)) remoteAction = "deactivate";
        else if ("settings".equals(action) || "setParameters".equals(action)) remoteAction = "update";
        else throw new IllegalArgumentException("This command controls the shared receiver, not a VFO");
        if ("deactivate".equals(remoteAction) && !confirmed.containsKey(selected))
            throw new IllegalStateException("This VFO has not been started");
        JSONObject message = base(remoteAction, selected);
        if (!"deactivate".equals(remoteAction)) {
            JSONObject values = valuesFrom(settings);
            desired.put(selected, values);
            message.put("settings", copy(values));
        }
        if ("activate".equals(remoteAction)) resume.put(selected, true);
        if ("deactivate".equals(remoteAction)) resume.put(selected, false);
        requests.put(message.getString("requestId"), selected);
        while (requests.size() > 64) requests.remove(requests.keySet().iterator().next());
        return message;
    }
    synchronized List<JSONObject> acceptState(JSONObject message) throws JSONException {
        List<JSONObject> restore = new ArrayList<>();
        String action = message.optString("action");
        if ("error".equals(action)) {
            String id = requests.remove(message.optString("requestId"));
            if (id != null) {
                JSONObject previous = confirmed.get(id);
                resume.put(id, previous != null && previous.optBoolean("active"));
                if (previous != null) desired.put(id, copy(previous));
            }
            return restore;
        }
        if (message.optJSONObject("capabilities") == null) return restore;
        String nextSession = message.optString("sessionId");
        if (nextSession.isEmpty()) return restore;
        boolean first = !ready || !session.equals(nextSession);
        session = nextSession;
        capabilities = copy(message.getJSONObject("capabilities"));
        ready = true;
        JSONArray vfos = message.optJSONArray("vfos");
        confirmed.clear();
        if (vfos != null) for (int i = 0; i < vfos.length(); ++i) {
            JSONObject state = vfos.optJSONObject(i);
            if (state == null) continue;
            String id = state.optString("vfoId");
            if (!"A".equals(id) && !"B".equals(id)) continue;
            confirmed.put(id, copy(state));
            desired.put(id, copy(state));
        }
        for (String id : new String[]{"A", "B"}) if (!desired.containsKey(id)) {
            RadioSettings defaults = new RadioSettings();
            defaults.listeningFrequency = capabilities.optDouble("centerFrequency", 100000000.0);
            defaults.modulationType = RadioSettings.MOD_NFM;
            defaults.bandwidth = 12500;
            defaults.fftLength = 2048;
            desired.put(id, valuesFrom(defaults));
        }
        if (first) for (Map.Entry<String, Boolean> entry : resume.entrySet()) {
            if (!entry.getValue()) continue;
            JSONObject activate = base("activate", entry.getKey());
            activate.put("settings", copy(desired.get(entry.getKey())));
            requests.put(activate.getString("requestId"), entry.getKey());
            restore.add(activate);
        }
        return restore;
    }
    synchronized boolean acceptsFrame(JSONObject header) {
        return ready && selected.equals(header.optString("vfoId")) &&
                session.equals(header.optString("sessionId"));
    }
    private JSONObject base(String action, String id) throws JSONException {
        return new JSONObject().put("type", "remoteVfo").put("action", action)
                .put("vfoId", id).put("clientName", "Obrii Android")
                .put("requestId", "vfo-" + (++counter));
    }
    private static JSONObject valuesFrom(RadioSettings settings) throws JSONException {
        if (!Double.isFinite(settings.listeningFrequency) || !Double.isFinite(settings.bandwidth))
            throw new IllegalArgumentException("VFO frequency and bandwidth must be finite");
        if (settings.modulationType < RadioSettings.MOD_AM || settings.modulationType > RadioSettings.MOD_WFM)
            throw new IllegalArgumentException("Remote VFO supports AM, NFM, SAM, USB, LSB, DSB, CW and WFM");
        return new JSONObject().put("frequencyHz", settings.listeningFrequency)
                .put("bandwidthHz", Math.max(100, Math.min(250000, settings.bandwidth)))
                .put("modulationType", settings.modulationType)
                .put("audioEnabled", settings.audioEnabled).put("spectrumEnabled", true)
                .put("fftLength", Math.max(512, Math.min(65536, settings.fftLength)))
                .put("refreshMs", 100).put("volume", 0.7);
    }
    private static JSONObject copy(JSONObject value) {
        try { return value == null ? new JSONObject() : new JSONObject(value.toString()); }
        catch (JSONException impossible) { throw new IllegalStateException(impossible); }
    }
}
