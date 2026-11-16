package com.fobosapp.networkclient;

import org.junit.Test;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class HackRfOtgProtocolTest {
    @Test
    public void recognisesPublicHackRfIdsButNotRad1oOrUnknownDevices() {
        assertTrue(HackRfOtgProtocol.isSupportedDevice(0x1d50, 0x6089));
        assertTrue(HackRfOtgProtocol.isSupportedDevice(0x1d50, 0x604b));
        assertFalse(HackRfOtgProtocol.isSupportedDevice(0x1d50, 0xcc15));
        assertFalse(HackRfOtgProtocol.isSupportedDevice(0x16d0, 0x132e));
    }

    @Test
    public void frequencyPayloadUsesMegahertzAndRemainderLittleEndian() {
        assertArrayEquals(new byte[] {(byte) 149, 0, 0, 0, (byte) 0xE0, 0x1F, 0x00, 0x00},
                HackRfOtgProtocol.frequencyPayload(149_008_160L));
    }

    @Test
    public void sampleRatePayloadUsesDividerOneAndOfficialFilterSelection() {
        assertArrayEquals(new byte[] {0x00, 0x12, 0x7A, 0x00, 1, 0, 0, 0},
                HackRfOtgProtocol.sampleRatePayload(8_000_000));
        assertEquals(6_000_000, HackRfOtgProtocol.automaticFilterBandwidthHz(8_000_000));
        assertEquals(15_000_000, HackRfOtgProtocol.automaticFilterBandwidthHz(20_000_000));
    }

    @Test
    public void clampsSafeFrequencySampleRateAndGainRanges() {
        assertEquals(1_000_000L, HackRfOtgProtocol.normalizeFrequencyHz(1.0));
        assertEquals(6_000_000_000L, HackRfOtgProtocol.normalizeFrequencyHz(7_000_000_000.0));
        assertEquals(2_000_000, HackRfOtgProtocol.normalizeSampleRateHz(100_000.0));
        assertEquals(20_000_000, HackRfOtgProtocol.normalizeSampleRateHz(28_000_000.0));
        assertEquals(24, HackRfOtgProtocol.lnaGainDbForUi(3));
        assertEquals(62, HackRfOtgProtocol.vgaGainDbForUi(50));
    }
}
