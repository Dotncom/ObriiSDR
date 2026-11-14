#!/usr/bin/env python3
"""Compare two B210 FPGA images in temporary UHD receive sessions on Linux."""

import argparse
import ctypes as c
import ctypes.util
import hashlib
import json
import math
import multiprocessing
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time


COUNTERS = ("overflows", "timeouts", "timestampGaps", "missingSamplesPerChannel",
            "blocksWithoutTimestamp")
BLOCK_SAMPLES = 65536
USB_BUFFERS = 256


def log(message):
    try:
        print(message, file=sys.stderr, flush=True)
    except BrokenPipeError:
        # An SSH client may disconnect before the baseline has been restored.
        pass


def file_path(value):
    path = Path(value).expanduser().resolve(strict=True)
    if not path.is_file():
        raise ValueError(f"Expected a regular file: {path}")
    return path


def image_path(value):
    path = file_path(value)
    if path.suffix.lower() != ".bin" or any(char in str(path) for char in ",=\r\n"):
        raise ValueError("Use a .bin path without commas, equals signs or line breaks (UHD arguments)")
    return path


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        return digest.hexdigest()


def json_lines(output):
    events = []
    for line in output.splitlines():
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def execute(args, env, timeout):
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, errors="replace", env=env, start_new_session=True,
                          restore_signals=True) as child:
        try:
            stdout, stderr = child.communicate(timeout=timeout)
        except (subprocess.TimeoutExpired, InterruptedError, KeyboardInterrupt):
            child.terminate()
            try:
                child.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
            raise
        return child.returncode, json_lines(stdout), stderr


def diagnostic_number(value):
    return type(value) in (float, int) and math.isfinite(value) and value >= 0


def summarize_rx(events, exit_code, rate, seconds):
    started = [event for event in events if event.get("event") == "started"]
    stopped = [event for event in events if event.get("event") == "stopped"]
    last = stopped[-1] if stopped else {}
    settings = started[-1] if started else {}
    counters = last.get("uhdRxDiagnostics")
    source = "stopped" if isinstance(counters, dict) else "unavailable"
    intervals = [event["uhdRxDiagnostics"] for event in events
                 if event.get("event") == "progress"
                 and isinstance(event.get("uhdRxDiagnostics"), dict)]
    if source == "unavailable" and intervals:
        counters, source = intervals[-1], "progress"
    counters = {key: counters[key] for key in (*COUNTERS, "maxReadPauseUs")
                if isinstance(counters, dict) and diagnostic_number(counters.get(key))}
    if source == "progress":
        # Older CLIs reset the maximum pause counter after each report.
        pauses = [item["maxReadPauseUs"] for item in intervals
                  if diagnostic_number(item.get("maxReadPauseUs"))]
        if pauses:
            counters["maxReadPauseUs"] = max(pauses)
    actual_rate = settings.get("sampleRateHz")
    samples = last.get("samples")
    elapsed = last.get("elapsedSeconds")
    receive_elapsed = last.get("receiveElapsedSeconds")
    channels = last.get("rxChannels")
    errors = []
    if exit_code:
        errors.append(f"ObriiCli returned {exit_code}")
    if len(started) != 1 or len(stopped) != 1 or channels != [0, 1]:
        errors.append("Expected a complete two-channel RX session")
    if not diagnostic_number(actual_rate) or abs(actual_rate - rate) > 1:
        errors.append("Reported sample rate differs from requested rate")
    if not diagnostic_number(samples) or (
            diagnostic_number(actual_rate) and samples < math.ceil(actual_rate * seconds - 1e-5)):
        errors.append("Insufficient received samples")
    complete = not errors
    if not all(key in counters for key in COUNTERS):
        errors.append("IQ diagnostics unavailable or incomplete")
    elif any(counters[key] for key in COUNTERS):
        errors.append("UHD reported overflow, timeout or IQ timestamp discontinuity")
    return {
        "state": "failed" if errors else ("partial" if source == "progress" else "clean"),
        "completed": complete,
        "actualRateHz": actual_rate, "actualCenterHz": settings.get("centerFrequencyHz"),
        "channels": channels, "samplesPerChannel": samples, "elapsedSeconds": elapsed,
        "receiveElapsedSeconds": receive_elapsed,
        "readRateMsps": (samples / receive_elapsed / 1e6
                          if diagnostic_number(samples) and diagnostic_number(receive_elapsed) and receive_elapsed > 0
                          else None),
        "diagnosticsSource": source, "diagnostics": counters, "errors": errors,
    }


def assert_idle(cli):
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            process_name = path.read_text().strip()
            if process_name in ("ObriiSDR", "ObriiCli", cli.name[:15]):
                raise RuntimeError(f"Close {process_name} on the B210 host before testing")
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue


def discover_device(cli, env, expected_serial):
    code, events, diagnostic = execute([cli, "devices", "--backend", "uhd"], env, 15)
    devices = []
    for event in events:
        description = event.get("deviceArgs")
        if not isinstance(description, str) or type(event.get("index")) is not int:
            continue
        fields = {key.strip(): value.strip() for pair in description.split(",") if "=" in pair
                  for key, value in (pair.split("=", 1),)}
        serial = fields.get("serial", "")
        if (fields.get("type") == "b200" and fields.get("product", "").upper() == "B210" and
                re.fullmatch(r"[a-zA-Z0-9_-]+", serial) and
                (not expected_serial or serial == expected_serial)):
            devices.append((event["index"], serial))
    if code or len(devices) != 1:
        if diagnostic.strip():
            log(diagnostic.strip()[-1800:])
        raise RuntimeError("Expected exactly one matching B210 (UHD type=b200, product=B210)")
    return devices[0]


def receive(args, env, device, image):
    with socket.socket() as port:
        port.bind(("127.0.0.1", 0))
        control_port = str(port.getsockname()[1])
    command = [args.cli, "run", "--backend", "uhd", "--device", str(device),
               "--rx-channel", "0", "--rx-dual", "--rate", str(args.rate),
               "--freq", str(args.center), "--seconds", str(args.seconds),
               "--gain-db", str(args.gain), "--antenna", args.antenna,
               "--block-samples", str(BLOCK_SAMPLES), "--fpga-image", image,
               "--clock-source", "internal", "--control-port", control_port, "--progress"]
    try:
        code, events, diagnostic = execute(command, env, args.seconds + 105)
    except subprocess.TimeoutExpired:
        return {"state": "failed", "errors": ["ObriiCli RX exceeded its deadline"]}
    if diagnostic.strip():
        log(diagnostic.strip()[-4000:])
    return summarize_rx(events, code, args.rate, args.seconds)


def nmea_records(payload, name):
    for line in payload.splitlines():
        message, separator, checksum = line.strip().partition(b"*")
        if not separator or len(checksum) != 2 or not message.startswith(b"$"):
            continue
        value = 0
        for byte in message[1:]:
            value ^= byte
        try:
            matches = value == int(checksum, 16)
        except ValueError:
            continue
        fields = message.split(b",")
        if matches and len(fields[0]) == 6 and fields[0][3:] == name:
            yield fields


def summarize_nmea(sentences):
    records = {name: list(nmea_records(sentences.get(name, b""), name))
               for name in (b"GGA", b"RMC", b"GSV")}
    gga = records[b"GGA"][-1] if records[b"GGA"] else []
    rmc = records[b"RMC"][-1] if records[b"RMC"] else []
    fix_quality = int(gga[6]) if len(gga) > 7 and gga[6].isdigit() else None
    satellites = int(gga[7]) if len(gga) > 7 and gga[7].isdigit() else None
    visible = [int(row[3]) for row in records[b"GSV"] if len(row) > 3 and row[3].isdigit()]
    received = any(records.values())
    rmc_active = len(rmc) > 2 and rmc[2] == b"A"
    fix = bool((fix_quality and fix_quality > 0) or rmc_active)
    return {
        "state": "fix-reported" if fix else ("nmea-no-fix" if received else "no-nmea"),
        "fixReported": fix, "fixQuality": fix_quality, "satellitesUsed": satellites,
        "satellitesInViewMaxPerTalker": max(visible) if visible else None,
        "sentences": {name.decode(): len(rows) for name, rows in records.items()},
    }


def probe_gnss(connection, library, address, check_pps):
    """Runs in a separate process so a stalled UHD call has a deadline."""
    os.setsid()
    for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, signal.SIG_DFL)
    result = {"gnss": {"state": "unavailable"}, "pps": {"state": "not-requested"}}
    try:
        lib = c.CDLL(library)
        handle = c.c_void_p
        pointer = c.POINTER(handle)
        signatures = {
            "usrp_make": [pointer, c.c_char_p], "usrp_free": [pointer],
            "usrp_get_mboard_sensor": [handle, c.c_char_p, c.c_size_t, pointer],
            "sensor_value_make": [pointer], "sensor_value_free": [pointer],
            "sensor_value_value": [handle, c.c_char_p, c.c_size_t],
        }
        if check_pps:
            signatures.update({
                "usrp_set_clock_source": [handle, c.c_char_p, c.c_size_t],
                "usrp_get_time_source": [handle, c.c_size_t, c.c_char_p, c.c_size_t],
                "usrp_set_time_source": [handle, c.c_char_p, c.c_size_t],
                "usrp_get_time_last_pps": [handle, c.c_size_t, c.POINTER(c.c_int64),
                                            c.POINTER(c.c_double)],
            })
        for name, argtypes in signatures.items():
            function = getattr(lib, "uhd_" + name)
            function.argtypes, function.restype = argtypes, c.c_int

        device = handle()
        code = lib.uhd_usrp_make(c.byref(device), os.fsencode(address))
        if code:
            raise RuntimeError(f"UHD sensor session open returned {code}")
        try:
            if check_pps:
                try:
                    result["pps"] = measure_pps(lib, device)
                except RuntimeError as error:
                    result["pps"] = {"state": "unavailable", "error": str(error)}
            else:
                time.sleep(3)
            sentences = {}
            for kind, names in ((b"GGA", (b"gnss_gga", b"gps_gpgga")),
                                (b"RMC", (b"gnss_rmc", b"gps_gprmc")),
                                (b"GSV", (b"gnss_gsv",))):
                for name in names:
                    sensor, data = handle(), c.create_string_buffer(32768)
                    if lib.uhd_sensor_value_make(c.byref(sensor)):
                        continue
                    try:
                        if (not lib.uhd_usrp_get_mboard_sensor(device, name, 0, c.byref(sensor))
                                and not lib.uhd_sensor_value_value(sensor, data, len(data))):
                            sentences[kind] = data.value
                            if data.value:
                                break
                    finally:
                        lib.uhd_sensor_value_free(c.byref(sensor))
            result["gnss"] = summarize_nmea(sentences)
            if not sentences:
                result["gnss"]["state"] = "sensor-unavailable"
        finally:
            lib.uhd_usrp_free(c.byref(device))
    except (OSError, AttributeError, RuntimeError) as error:
        result["gnss"] = {"state": "unavailable", "error": str(error)}
        if check_pps:
            result["pps"] = {"state": "unavailable", "error": str(error)}
    connection.send(result)
    connection.close()


def measure_pps(lib, device):
    def checked(code, operation):
        if code:
            raise RuntimeError(f"{operation} returned {code}")

    def sample():
        whole, fraction = c.c_int64(), c.c_double()
        checked(lib.uhd_usrp_get_time_last_pps(device, 0, c.byref(whole), c.byref(fraction)),
                "Read last PPS")
        return whole.value + fraction.value

    readback = c.create_string_buffer(128)
    checked(lib.uhd_usrp_get_time_source(device, 0, readback, len(readback)), "Read time source")
    previous_source = readback.value
    def observe(source, seconds):
        checked(lib.uhd_usrp_set_time_source(device, source, 0), "Select " + source.decode() + " PPS")
        checked(lib.uhd_usrp_get_time_source(device, 0, readback, len(readback)), "Read GPS PPS source")
        if readback.value != source:
            raise RuntimeError("PPS time-source readback did not match")
        time.sleep(0.25)
        previous = sample()
        edges = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = sample()
            if value != previous:
                edges.append(value)
                previous = value
            time.sleep(0.025)
        periods = [round(next_edge - edge, 8) for edge, next_edge in zip(edges, edges[1:])]
        return len(edges), periods

    def stable(count, periods):
        return count >= 3 and all(abs(period - 1) < 0.005 for period in periods)

    checked(lib.uhd_usrp_set_clock_source(device, b"internal", 0), "Use internal reference")
    try:
        none_edges, _ = observe(b"none", 2)
        internal_edges, internal_periods = observe(b"internal", 4)
        edges, periods = observe(b"gpsdo", 8)
        controls = none_edges == 0 and stable(internal_edges, internal_periods)
        state = ("stable" if stable(edges, periods) else ("unstable" if edges else "no-edges"))
        return {"state": state if controls else "control-failed", "edges": edges,
                "periodsSeconds": periods, "disabledEdges": none_edges,
                "internalReferenceEdges": internal_edges, "controlsPassed": controls}
    finally:
        checked(lib.uhd_usrp_set_time_source(device, previous_source, 0), "Restore time source")


def read_sensors(library, address, check_pps):
    ctx = multiprocessing.get_context("fork")
    receiver, sender = ctx.Pipe(duplex=False)
    child = ctx.Process(target=probe_gnss, args=(sender, library, address, check_pps))
    child.start()
    sender.close()
    try:
        if receiver.poll(55):
            try:
                return receiver.recv()
            except EOFError:
                message = "UHD sensor helper exited before reporting"
        else:
            message = "UHD sensor session exceeded its deadline"
        return {
            "gnss": {"state": "unavailable", "error": message},
            "pps": {"state": "unavailable" if check_pps else "not-requested"},
        }
    finally:
        receiver.close()
        child.join(timeout=1)
        if child.is_alive():
            child.kill()
            child.join()


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, help="Path to ObriiCli on the B210 host")
    parser.add_argument("--baseline", required=True, help="Known-good dual RX UHD .bin")
    parser.add_argument("--candidate", required=True, help="Candidate dual RX UHD .bin")
    parser.add_argument("--expected-candidate-sha256", help="Require this hash before opening UHD")
    parser.add_argument("--rate", type=int, default=30720000, help="Samples per second per channel")
    parser.add_argument("--center", type=int, default=100000000, help="RF center in Hz")
    parser.add_argument("--seconds", type=int, default=15, help="IQ duration per image (2–300 s)")
    parser.add_argument("--gain", type=float, default=10)
    parser.add_argument("--antenna", choices=("RX2", "TX/RX"), default="RX2")
    parser.add_argument("--library", help="Optional UHD shared library for this clone")
    parser.add_argument("--images-dir", help="Location of UHD USB firmware and standard images")
    parser.add_argument("--uhd-probe", default="uhd_usrp_probe", help="UHD utility for restoring baseline")
    parser.add_argument("--expect-serial", help="Require this B210 serial number for the diagnostics")
    parser.add_argument("--gnss-baud", type=int, choices=(9600, 38400))
    parser.add_argument("--check-pps", action="store_true", help="Measure GPS PPS on candidate")
    return parser.parse_args(argv)


def run(args, report):
    if not sys.platform.startswith("linux"):
        raise ValueError("Run the hardware check on the Linux host connected to B210")
    import fcntl

    if (not 1 <= args.rate <= 30720000 or not 1 <= args.center <= 6000000000
            or not 2 <= args.seconds <= 300 or not 0 <= args.gain <= 76):
        raise ValueError("Check rate, center, duration and gain against the B210 settings")
    args.cli = file_path(args.cli)
    args.baseline, args.candidate = image_path(args.baseline), image_path(args.candidate)
    for name in ("baseline", "candidate"):
        path = getattr(args, name)
        if path.stat().st_size == 0:
            raise ValueError(f"The {name} image is empty")
        report[name] = {"sha256": sha256(path)}
    expected = args.expected_candidate_sha256
    if expected and report["candidate"]["sha256"] != expected.lower():
        raise ValueError("Candidate SHA-256 does not match")
    report["imagesIdentical"] = report["baseline"]["sha256"] == report["candidate"]["sha256"]
    probe = shutil.which(args.uhd_probe)
    if not probe or not os.access(args.cli, os.X_OK):
        raise ValueError("An executable ObriiCli and uhd_usrp_probe are required")
    env = os.environ.copy()
    library = args.library or env.get("OBRII_UHD_LIBRARY_PATH")
    if library:
        library = str(file_path(library))
        env["OBRII_UHD_LIBRARY_PATH"] = library
        parts = [str(Path(library).parent), env.get("LD_LIBRARY_PATH", "")]
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, parts))
    env["OBRII_UHD_NUM_RECV_FRAMES"] = str(USB_BUFFERS)
    if args.images_dir:
        images = Path(args.images_dir).expanduser().resolve(strict=True)
        if not images.is_dir():
            raise ValueError("UHD image directory is not a directory")
        env["UHD_IMAGES_DIR"] = str(images)
        # ctypes runs in a fork of this process and needs the same firmware path.
        os.environ["UHD_IMAGES_DIR"] = str(images)
    report["settings"] = {"rateHz": args.rate, "centerHz": args.center, "seconds": args.seconds,
                          "gainDb": args.gain, "antenna": args.antenna, "blockSamples": BLOCK_SAMPLES,
                          "usbBuffers": USB_BUFFERS}
    cache = Path(os.environ.get("XDG_RUNTIME_DIR") or Path.home() / ".cache") / "obrii"
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / "b210-fpga-check.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert_idle(args.cli)
        _, serial = discover_device(args.cli, env, args.expect_serial)
        address = f"type=b200,serial={serial}"
        changed = False
        try:
            for label in ("baseline", "candidate"):
                assert_idle(args.cli)
                current_index, _ = discover_device(args.cli, env, serial)
                log(f"Starting {label} RX at {args.rate / 1e6:g} MS/s per channel")
                changed = True
                report[label]["rx"] = receive(args, env, current_index, getattr(args, label))
                log(f"{label} RX: {report[label]['rx']['state']}")
                if label == "baseline" and not report[label]["rx"].get("completed"):
                    raise RuntimeError("Baseline RX did not complete")
            if (args.gnss_baud or args.check_pps) and report["candidate"]["rx"]["state"] != "failed":
                log("Checking candidate GNSS and PPS sensors")
                sensor_library = library or ctypes.util.find_library("uhd")
                if sensor_library:
                    sensor_address = f"{address},fpga={args.candidate}"
                    if args.gnss_baud:
                        sensor_address += f",b210_gnss_baud={args.gnss_baud}"
                    report.update(read_sensors(sensor_library, sensor_address, args.check_pps))
                else:
                    report["gnss"] = {"state": "unavailable", "error": "UHD library was not found"}
                    if args.check_pps:
                        report["pps"] = {"state": "unavailable"}
        finally:
            if changed:
                report["restored"] = restore_baseline(probe, address, args.baseline, env)
    states = [report[label]["rx"]["state"] for label in ("baseline", "candidate")]
    report["ok"] = report["restored"] and states == ["clean", "clean"]
    report["comparison"] = "clean" if report["ok"] else (
        "partial" if "failed" not in states and report["restored"] else "failed")
    return 0 if report["ok"] else 1


def restore_baseline(probe, address, baseline, env):
    # Complete the restoration even if the SSH client disconnects during cleanup.
    guarded = [signal.SIGHUP, signal.SIGTERM, signal.SIGINT]
    previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in guarded}
    try:
        log("Restoring the baseline image through UHD")
        for _ in range(2):
            try:
                code, _, diagnostic = execute([probe, "--args", f"{address},fpga={baseline}"], env, 55)
                if not code:
                    return True
                if diagnostic.strip():
                    log(diagnostic.strip()[-1800:])
            except (OSError, subprocess.TimeoutExpired) as error:
                log(str(error))
        return False
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def interrupt(_signal, _frame):
    raise InterruptedError("B210 check interrupted")


def main(argv=None):
    args = parse_args(argv)
    report = {"schema": 1, "ok": False, "comparison": "not-run", "restored": None,
              "gnss": {"state": "not-requested"}, "pps": {"state": "not-requested"}}
    if sys.platform.startswith("linux"):
        for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, interrupt)
    try:
        status = run(args, report)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
        status = 1 if report["restored"] is not None else 2
        if status == 1:
            report["comparison"] = "failed"
    try:
        print(json.dumps(report, separators=(",", ":")), flush=True)
    except BrokenPipeError:
        pass
    return status


if __name__ == "__main__":
    sys.exit(main())
