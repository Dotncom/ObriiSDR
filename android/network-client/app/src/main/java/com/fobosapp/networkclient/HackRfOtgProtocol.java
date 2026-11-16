package com.fobosapp.networkclient;

/**
 * Small, Android-free description of the public HackRF USB RX protocol.
 *
 * <p>Keeping byte layouts here lets the OTG transport be checked with ordinary JVM
 * tests.  It intentionally covers RX only; this application must never start a
 * transmitter merely because an Android USB device was connected.</p>
 */
final class HackRfOtgProtocol {
    static final int VENDOR_ID = 0x1d50;
    static final int PRODUCT_ID_JAWBREAKER = 0x604b;
    static final int PRODUCT_ID_HACKRF_ONE = 0x6089;

    static final int REQUEST_SET_TRANSCEIVER_MODE = 1;
    static final int REQUEST_SAMPLE_RATE_SET = 6;
    static final int REQUEST_BASEBAND_FILTER_BANDWIDTH_SET = 7;
    static final int REQUEST_BOARD_ID_READ = 14;
    static final int REQUEST_VERSION_STRING_READ = 15;
    static final int REQUEST_SET_FREQ = 16;
    static final int REQUEST_AMP_ENABLE = 17;
    static final int REQUEST_SET_LNA_GAIN = 19;
    static final int REQUEST_SET_VGA_GAIN = 20;

    static final int TRANSCEIVER_MODE_OFF = 0;
    static final int TRANSCEIVER_MODE_RECEIVE = 1;

    static final long MIN_FREQUENCY_HZ = 1_000_000L;
    static final long MAX_FREQUENCY_HZ = 6_000_000_000L;
    static final int MIN_SAMPLE_RATE_HZ = 2_000_000;
    static final int MAX_SAMPLE_RATE_HZ = 20_000_000;

    private static final int[] FILTER_BANDWIDTHS_HZ = {
            1_750_000, 2_500_000, 3_500_000, 5_000_000,
            5_500_000, 6_000_000, 7_000_000, 8_000_000,
            9_000_000, 10_000_000, 12_000_000, 14_000_000,
            15_000_000, 20_000_000, 24_000_000, 28_000_000
    };

    private HackRfOtgProtocol() {
    }

    static boolean isSupportedDevice(int vendorId, int productId) {
        return vendorId == VENDOR_ID &&
                (productId == PRODUCT_ID_JAWBREAKER || productId == PRODUCT_ID_HACKRF_ONE);
    }

    static long normalizeFrequencyHz(double frequencyHz) {
        if (!Double.isFinite(frequencyHz)) {
            return 100_000_000L;
        }
        long rounded = Math.round(frequencyHz);
        return Math.max(MIN_FREQUENCY_HZ, Math.min(MAX_FREQUENCY_HZ, rounded));
    }

    static int normalizeSampleRateHz(double sampleRateHz) {
        if (!Double.isFinite(sampleRateHz)) {
            return 8_000_000;
        }
        int rounded = (int) Math.round(sampleRateHz);
        return Math.max(MIN_SAMPLE_RATE_HZ, Math.min(MAX_SAMPLE_RATE_HZ, rounded));
    }

    /** Encodes HackRF SET_FREQ: uint32 MHz followed by uint32 Hz remainder, LE. */
    static byte[] frequencyPayload(long frequencyHz) {
        long normalized = Math.max(MIN_FREQUENCY_HZ, Math.min(MAX_FREQUENCY_HZ, frequencyHz));
        long megahertz = normalized / 1_000_000L;
        long remainderHz = normalized % 1_000_000L;
        byte[] payload = new byte[8];
        writeLe32(payload, 0, megahertz);
        writeLe32(payload, 4, remainderHz);
        return payload;
    }

    /** Encodes HackRF SAMPLE_RATE_SET for an integral sample rate (divider = 1). */
    static byte[] sampleRatePayload(int sampleRateHz) {
        byte[] payload = new byte[8];
        writeLe32(payload, 0, normalizeSampleRateHz(sampleRateHz));
        writeLe32(payload, 4, 1);
        return payload;
    }

    /** Selects the widest official analogue filter no wider than 75% of Fs. */
    static int automaticFilterBandwidthHz(int sampleRateHz) {
        int target = (int) Math.floor(normalizeSampleRateHz(sampleRateHz) * 0.75);
        int selected = FILTER_BANDWIDTHS_HZ[0];
        for (int bandwidth : FILTER_BANDWIDTHS_HZ) {
            if (bandwidth <= target) {
                selected = bandwidth;
            } else {
                break;
            }
        }
        return selected;
    }

    static int lnaGainDbForUi(int uiGain) {
        int clamped = Math.max(0, Math.min(3, uiGain));
        return clamped * 8;
    }

    static int vgaGainDbForUi(int uiGain) {
        int clamped = Math.max(0, Math.min(31, uiGain));
        return clamped * 2;
    }

    private static void writeLe32(byte[] destination, int offset, long value) {
        destination[offset] = (byte) (value & 0xff);
        destination[offset + 1] = (byte) ((value >>> 8) & 0xff);
        destination[offset + 2] = (byte) ((value >>> 16) & 0xff);
        destination[offset + 3] = (byte) ((value >>> 24) & 0xff);
    }
}
