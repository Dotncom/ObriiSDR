# Multi-user Remote VFO

Obrii SDR can expose one running wideband receiver to several independent
listeners without giving every listener control of the physical radio. This is
the lightweight multi-user path; it is separate from Full IQ streaming and from
the external DSP compute worker.

## Server

1. Start the physical receiver normally on the server computer.
2. Open **Network**, select **Server**, and apply the desired bind address and
   control port.
3. Enable **Browser Remote VFO interface** and select its port if browser access
   is needed. The browser URL is shown in the status area. When binding to
   `0.0.0.0`, replace that part of the shown URL with the server's LAN address.

The browser service is disabled by default. It has no built-in user database,
TLS, or Internet-facing authentication, so expose it only on a trusted LAN or
through a VPN/authenticated TLS reverse proxy.

The browser header contains a full captured-span slider. The separate scale
between spectrum and waterfall shows the current narrow VFO span with frequency
labels and a red tuning marker. Click or drag on the spectrum, scale or
waterfall to tune inside that span. The mouse wheel tunes by one twentieth of
the selected bandwidth; Ctrl gives a ten-times finer step and Shift a ten-times
coarser step. Typing MHz updates an active VFO after a short debounce rather
than waiting for the field to lose focus.

## Native client

Select **Client** and **Independent server VFO (multi-user)**, connect to the
normal Obrii control port, choose a listening frequency inside the server's
currently captured RF span, then press Start. Frequency, demodulation,
bandwidth, squelch, audio and the narrow spectrum belong to that client. Stop
releases the active branch but keeps the network connection.

The Network dialog stores named client presets (name, host/IP and control port)
in the local user INI. Double-click a row or press **Use selected** to fill the
connection fields. These private addresses are not compiled into Obrii and are
not included in source or release packages.

In normal **Server processing** mode zooming and panning are independent of the
server UI. The native client sends a viewport center and span; the server crops
that window from the complete FFT before the normal 2048-bin network reduction.
This retains zoom detail without retuning the receiver. Only crossing the
captured RF edge requests a physical center-frequency change. A new network
frame therefore no longer snaps the client back to the server window. The
server option that disables its local audio/video is host-owned and cannot be
overwritten by a client's settings.

The server currently permits eight active remote VFOs in total and two per
session. An independent VFO cannot retune, stop, or transmit through the shared
physical receiver. The server operator remains responsible for receiver center
frequency, sample rate and TX safety. A requested VFO outside the captured span
is rejected without changing the previous valid VFO state.

The first native client connected to an otherwise empty control server becomes
the controller. Further clients are observers. If the controller disconnects,
already connected observers remain observers instead of inheriting hardware
control silently; one of them must use **Request control**. This also prevents
an older client joining during a transient disconnect from taking over the
receiver automatically. Commands sent by an observer are rejected by the
server even if an older client UI does not understand the role indicator.

## Processing and isolation

Each active VFO is a bounded DDC/demodulation branch. The shared receiver IQ is
published once; each listener receives only its own 48 kHz mono PCM and narrow
FFT frames. Remote VFO sockets are excluded from the legacy full-spectrum/IQ
broadcast and from its back-pressure calculation. A slow browser or listener
therefore drops its own queued output instead of blocking local audio or other
clients.

When a native external-compute endpoint is enabled in the DSP stream manager,
remote VFO branches may use it. Local acquisition and the established main
audio path remain independent of network waits. This is isolation rather than
a hard real-time guarantee: all branches still share receiver throughput,
memory bandwidth and the configured compute budget.

The browser uses a small built-in HTTP/WebSocket gateway and requires no extra
package. `/api/status` returns non-sensitive service counters and capabilities.
Neither IQ nor location data is uploaded automatically.

## Current boundary

This first implementation provides independent receive sessions and a browser
spectrum/waterfall/audio client. It does not yet implement a public receiver
directory, account management, TCI/KiwiSDR/RTL-TCP compatibility, synchronized
hardware timestamps, or an Internet relay. Those belong after LAN qualification
on Raspberry Pi and the external compute node.
