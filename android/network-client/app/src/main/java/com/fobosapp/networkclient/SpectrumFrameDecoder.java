package com.fobosapp.networkclient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.math.BigDecimal;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class SpectrumFrameDecoder {
    static final int MAX_DISPLAY_BINS = 1024 * 1024;
    private static final long MAX_DISPLAY_BYTES = (long) MAX_DISPLAY_BINS * Float.BYTES * 2;

    private SpectrumFrameDecoder() { }

    // Validate before reading/allocating the payload, including the unused
    // reference plane. A missing FFT length retains the legacy inferred count.
    static int validateBinaryHeader(JSONObject header, long payloadBytes) throws IOException {
        if (payloadBytes <= 0 || payloadBytes > MAX_DISPLAY_BYTES)
            throw new IOException("Spectrum binary payload exceeds the display limit");
        if (header.has("payloadBytes") &&
                boundedInteger(header.opt("payloadBytes"), MAX_DISPLAY_BYTES, "payloadBytes") != payloadBytes)
            throw new IOException("Spectrum binary payload byte count does not match");
        int bytesPerBin = bytesPerBin(header);
        Object reference = header.opt("hasReferenceMagnitudes");
        if (reference != null && !(reference instanceof Boolean))
            throw new IOException("Invalid spectrum reference flag");
        long stride = (long) bytesPerBin * (Boolean.TRUE.equals(reference) ? 2 : 1);
        long count = header.has("fftLength")
                ? boundedInteger(header.opt("fftLength"), MAX_DISPLAY_BINS, "fftLength")
                : payloadBytes / stride;
        if (count <= 0 || count > MAX_DISPLAY_BINS || payloadBytes != count * stride)
            throw new IOException("Spectrum binary dimensions do not match its payload");
        return (int) count;
    }

    static float[] decodeBinary(JSONObject header, byte[] payload) throws IOException {
        if (payload == null) throw new IOException("Missing spectrum binary payload");
        int count = validateBinaryHeader(header, payload.length);
        boolean compact = bytesPerBin(header) == Short.BYTES;
        boolean ascending = isAscending(header);
        ByteBuffer buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        float[] magnitudes = new float[count];
        for (int i = 0; i < count; ++i) {
            int source = sourceIndex(i, count, ascending);
            float value = compact ? buffer.getShort(source * Short.BYTES) / 10.0f
                    : buffer.getFloat(source * Float.BYTES);
            magnitudes[i] = Float.isFinite(value) ? value : -160.0f;
        }
        return magnitudes;
    }

    static int validateJsonCount(JSONObject header) throws IOException {
        JSONArray frequencies = header.optJSONArray("frequencies");
        JSONArray magnitudes = header.optJSONArray("magnitudes");
        if (frequencies == null || magnitudes == null || frequencies.length() <= 0 ||
                frequencies.length() > MAX_DISPLAY_BINS || magnitudes.length() != frequencies.length())
            throw new IOException("Invalid spectrum JSON dimensions");
        return frequencies.length();
    }

    static boolean isAscending(JSONObject header) {
        return "ascending-frequency".equals(header.optString("magnitudeOrder"));
    }

    static int sourceIndex(int displayIndex, int count, boolean ascending) {
        if (ascending) return displayIndex;
        int source = displayIndex + (count + 1) / 2;
        return source < count ? source : source - count;
    }

    private static int bytesPerBin(JSONObject header) throws IOException {
        Object format = header.opt("binFormat");
        if (format == null || "f32le".equals(format)) return Float.BYTES;
        if ("s16le-db10".equals(format)) return Short.BYTES;
        throw new IOException("Unsupported spectrum binary format");
    }

    private static long boundedInteger(Object value, long maximum, String field) throws IOException {
        try {
            final long parsed;
            if (value instanceof Number) parsed = new BigDecimal(value.toString()).longValueExact();
            else if (value instanceof String) parsed = Long.parseLong((String) value);
            else throw new IOException("Invalid spectrum " + field);
            if (parsed < 0 || parsed > maximum) throw new IOException("Invalid spectrum " + field);
            return parsed;
        } catch (NumberFormatException | ArithmeticException invalid) {
            throw new IOException("Invalid spectrum " + field);
        }
    }
}
