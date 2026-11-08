# GNSS/QTH preflight for 4.1

This note is a practical checklist for the current GNSS workbench in Obrii SDR.
It is not a promise of autonomous GNSS positioning yet: real GPS L1 C/A lock is
still unproven with the current antenna/RF conditions.

## Recommended Test Order

1. Open `GPS/QTH`.
2. Press `Time` and verify that network/system time is available. This is only
   coarse assisted-GNSS context; it does not replace code-phase acquisition.
3. Press `Position test`. The synthetic pseudorange solver should update QTH and
   open the map with a synthetic marker.
4. Select `GPS L1 C/A` or `All L1`, then press the GNSS tune button.
5. Enable `GNSS IQ monitor` and wait a few seconds. Watch RMS, DC, clipping,
   I/Q balance, and spur logs.
6. Press `Save GNSS IQ` for a controlled recording.
7. Open playback, select the saved stereo Channel IQ WAV, and press `GPS replay`.
8. Compare live/replay/self-test reports in `recordings/gnss_reports`.

## Useful Settings

| Mode | Integration | Doppler span | Doppler step | Purpose |
| --- | ---: | ---: | ---: | --- |
| Fast smoke test | 8-24 ms | +/- 10 kHz | 1000 Hz | UI/path check |
| Normal replay | 24-64 ms | +/- 25 kHz | 500-1000 Hz | Saved IQ comparison |
| Deep replay | 96-200 ms | +/- 25 kHz | 250-500 Hz | Slow investigation |

Keep the channel filter wide enough for GPS C/A. Around 1.8 MHz is the current
default. Narrow audio-style bandwidths such as 20 kHz are not suitable for GPS
spread-spectrum acquisition.

On Standard firmware, 80 MS/s is not useful for this specific GNSS view because
the practical RF span edges are filtered; use the visible L1 span presets instead.
On Agile firmware, use the sample rates actually supported by that firmware.

## RF Notes

Phone lock is not a fair comparison. Phones use assisted GNSS, dedicated GNSS
front ends, filtering, low-noise amplification, and multiple constellations.

For real SDR acquisition, prefer:

- active GNSS antenna;
- safe external bias tee if the antenna needs power;
- short coax;
- clear sky view;
- tests away from USB/display/power noise;
- saved IQ recordings for replay.

True GPS L1 C/A often sits below the visible noise floor. Narrow stable lines near
1575.42 MHz are more likely spurs or interference than satellites.

## Generated Artifacts

Each completed GNSS acquisition path writes files to:

```text
recordings/gnss_reports/
```

The current artifacts are:

- `gnss_acq_<timestamp>_<source>.json` - summary, settings, QTH context, best
  candidates, likely-lock decision, and artifact file names;
- `gnss_acq_<timestamp>_<source>_heatmap.csv` - PRN/Doppler metric grid;
- `gnss_acq_<timestamp>_<source>_profile.csv` - best code-phase correlation
  profile.

The `source` field is currently `live`, `deep-live`, `replay`, or
`synthetic-iq`.

These files are meant for comparing antennas, gains, firmware, sample rates, and
future reference-tool runs such as GNSS-SDR.

## Current Stop Point

For the 4.1 draft, the useful goal is:

- QTH/map workflow usable;
- online/offline map layers usable;
- NMEA paste usable;
- synthetic acquisition and synthetic position tests pass;
- real IQ can be recorded, replayed, and exported with repeatable reports.

Real multi-satellite navigation remains a later step after RF reception improves
and a repeatable GPS L1 C/A lock is proven.
