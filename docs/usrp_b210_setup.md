# USRP B210 setup in Obrii SDR

Install UHD and the FPGA image for your board before connecting. On Raspberry
Pi OS, install `libuhd-dev` and `uhd-host`. For Windows or other systems, use
[the UHD installation guide](https://files.ettus.com/manual/page_install.html).
For a compatible Ettus board, use its standard UHD image. Kintex-7 clones may
need an image and UHD library supplied by the board vendor. Use
`uhd_find_devices` and `uhd_usrp_probe` to verify detection.

In Obrii select **USRP UHD native**, open **USRP UHD RX**, and select the
channel, antenna and gain. The optional FPGA path loads a compatible `.bin`
for the current session. For the standard UHD image, leave the field empty.
Stop and restart local RX after changing the image. A custom image can take
over 20 seconds to load. Set `OBRII_UHD_LIBRARY_PATH` to the absolute path
of a separately installed compatible SDK library when needed.

## Dual RX in Obrii CLI

With a two-channel B210 image, start one timed RX stream with `--rx-dual`.
Both inputs use the same center and sample rate, up to 30.72 MS/s per channel.
`--rx-channel` selects the primary input. `--rx-second-gain-db` and
`--rx-second-antenna` set the other input independently.

```sh
ObriiCli run --backend uhd --device 0 --rx-dual --rx-channel 0 --freq 101.287174M --rate 15.36M \
  --gain-db 35 --antenna RX2 --rx-second-gain-db 30 --rx-second-antenna RX2 --spectrum --serve
```

Add `--fpga-image` with a compatible two-channel image for the clone.
On the client, the second channel appears as a separate spectrum and waterfall
in Multi-SDR. Network audio and the existing IQ stream carry the primary
channel. With `--output FILE.cf32`, the CLI writes the other channel to
`FILE.cf32.rxN.cf32` using the same receive-block boundaries.

On ARM Linux, Obrii defaults to 256 in-flight USB receive buffers for UHD
B2xx. `OBRII_UHD_NUM_RECV_FRAMES` can override the default, and an explicit
UHD device argument takes precedence. GUI diagnostic logging reports UHD
overflows, timeouts and the longest pause between reads at Stop. In dual CLI
mode, `uhdRxDiagnostics` progress records also report timestamp gaps.

## UHD clock source

Open **USRP UHD RX** on the local receiver or on a client connected to
the Raspberry Pi CLI server. **Auto** preserves the driver's default.
**Internal** and **External 10 MHz** explicitly select the reference.
For local RX, use Stop and Start to apply a changed source. The server
reopens active RX when you click **Apply** and tries to restore the
previous settings if the new source is rejected.

The active reference is read back from UHD. With an external reference,
the dialog shows whether the `ref_locked` sensor reports lock. A missing
sensor is displayed separately from an unlocked reference. The CLI accepts
`--clock-source auto|internal|external` and reports `uhdClock` in
`control status`. This selection controls the frequency reference.

## Onboard GPS coordinates

In **GPS / QTH**, select **UHD GPS** as the position source and start RX.
The QTH status shows whether NMEA data is arriving. With a UHD library that
exposes GSV, it also shows satellites in view, satellites with measured signal,
and the strongest C/N₀. The GGA satellite count shows how many are used for the
fix. Once the module has a valid fix, the status shows coordinates and a
Maidenhead locator, and the QTH map displays the position. The displayed fix
is temporary.
Routine status reports and receiver logs omit the coordinates.

For the Kintex-7 board, use a GNSS-enabled FPGA image and a compatible UHD
library. Set the FPGA image and **GPS NMEA baud** to **38400** in
**USRP UHD RX** before starting local RX. Use **Auto (UHD sensors)** with a
standard UHD GPSDO. When installing a compatible UHD library separately,
set `OBRII_UHD_LIBRARY_PATH` to its absolute path before launching Obrii.

On the tested Raspberry setup, 240 polls returned GGA, RMC and GSV over
four minutes. GSV listed 25 satellites, with no measured C/N₀, and GGA
reported zero satellites used. The position fix remains unverified. GSV's
in-view count can be positive before any satellite signal is measured.

In **GPS / QTH**, source **UHD + serial GNSS** combines the satellite
observations and keeps each source distinct on the sky map. When validating
UHD fixes, select a reference Maidenhead grid, a maximum distance and an
allowed time difference. Enable display of rejected fixes to see a cross
and its rejection reason. Enable the corresponding logging option and
application logging to save the rejected GGA/RMC data locally.

For network use, start the UHD server with `--uhd-gnss-baud 38400` and
the corresponding `--fpga-image` and UHD library. Use `--uhd-gnss`
for a device with standard UHD GPSDO sensors. On a trusted network the
client with receiver control can select the same **UHD GPS** source.

With the optional UHD `gnss_ubx` sensor, `ObriiCli control uhd-gnss`
includes recent checksum-valid UBX frames in `ubxHex` when the onboard module
emits them on UART. The GPS panel shows the number of packets and includes
the hex frames in the local fix log when that log is enabled. The FPGA GNSS
path currently receives UART data and cannot configure the module's output.

## UHD output in the TX window

Open **TX…** and select **USRP UHD (local)**. Choose the device, TX channel,
gain and antenna. On B210, each TX channel uses its corresponding **TX/RX**
port. For simultaneous local RX and TX on that B210, stop RX, enable
**Simultaneous RX + TX (B210)** in **USRP UHD RX**, select 15.36 or 30.72 MS/s,
and start RX again. The RX path uses **RX2**. Select the same channel number
in the TX window and use **TX/RX** for TX. Local GUI duplex is one RX plus
one TX. When receiving through a separate Fobos, the standalone B210 TX
output is available without changing the Fobos RX settings.
The window sends generated IQ at 1.92 MS/s and accepts baseband rates from
8 kS/s to 1.92 MS/s. A custom FPGA image can be set in **USRP UHD RX**.

For a B210 on Raspberry Pi, run the updated **ObriiCli** server with
`--duplex-tx-channel 0` or `--duplex-tx-channel 1`. Choose an RX rate of
15.36 or 30.72 MS/s per channel and **RX2** on every receiving channel.
Use matching RX and TX channel numbers in 1+1 mode. With `--rx-dual` the
client can select either TX channel for the waveform.

For example, from the main installation's `build/linux-release` directory:

```sh
./start-obrii.sh --cli run --backend uhd --device 0 \
  --rx-channel 0 --antenna RX2 --duplex-tx-channel 0 \
  --rate 30.72M --freq 102.287174M --mode wfm --bandwidth 200k \
  --block-samples 65536 --spectrum --serve --bind 192.168.89.248 \
  --network-port 21090 --control-port 17675
```

For a B210 clone, append `--fpga-image` with the path to its compatible image.

In the controlling client, choose **USRP UHD (network)** in **TX…**. Set
the TX channel, gain and antenna, enter the single-use code described
below, and click **Start RF**. Start with a baseband
rate of 48 kS/s for narrowband signals. Network IQ uses four bytes per
complex sample. Upload needs about 1.54 Mbit/s at 48 kS/s or 61.44 Mbit/s
at 1.92 MS/s, before protocol overhead.
RX continues through the same UHD session. Use a trusted network.

To keep local Fobos reception on the same Obrii window, leave it in local
receive mode. In **TX… → USRP UHD (network)**, enter the B210 server address
and port and click **Connect**. This connection carries TX IQ and control
without subscribing to RX spectrum or audio. The server still grants
control to one client at a time.

### Remote test tone

On the Raspberry Pi, enable duplex test bursts with
`--duplex-tx-channel 0` or `--duplex-tx-channel 1`. Set the RX rate
to `15360000` or `30720000` per channel. RX and TX use one UHD
session and a fixed 30.72 MHz master clock.

- For **2 RX + 2 TX**, include `--rx-dual` and set both RX antennas
  to `RX2`. The TX option selects the default output in the client.
  Connect **both** TX/RX ports to 50-ohm loads or suitably attenuated test
  paths. UHD keeps both TX lanes enabled during a burst, including when
  Obrii submits zero IQ on one lane.
- For **1 RX + 1 TX**, use a matching pair such as
  `--rx-channel 0 --antenna RX2 --duplex-tx-channel 0`. Connect the
  corresponding TX/RX port to a 50-ohm load or attenuated test path.

From the Raspberry Pi, issue an arm code over the server's loopback
control port:

```sh
./ObriiCli control uhd-tx-arm --control-port 17675
```

In **TX… → USRP UHD (network)**, click **Test tone…**. Select TX 0, TX 1,
or both outputs when available. Enter the common TX center, gain (0–10 dB),
duration (50–500 ms), and single-use code. The code expires in 60 seconds.
The test sends a ramped tone 1 kHz above center on a single selected port.
With both outputs selected, TX 0 is 1 kHz above center and TX 1 is 2 kHz
above center. Each TX lane runs at 1.92 MS/s. **Stop TX** cancels the
burst. Use this test mode over a trusted network.

RX keeps running during the burst. The `uhdTx` event reports the selected
output, samples per channel submitted to UHD, and the RX overflow and
missing-sample counters for the burst window. RF output, TX underruns, and
RX continuity need hardware verification.
