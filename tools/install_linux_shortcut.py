#!/usr/bin/env python3
"""Install the Fusion 110% launcher for a built Obrii Linux application."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def write_file(path, text, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(text)
            stream.flush()
            temporary.chmod(mode)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def desktop_value(value):
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


def exec_argument(value):
    quoted = "".join("\\" + char if char in '\\"`$' else char for char in str(value))
    return '"' + desktop_value(quoted).replace("%", "%%") + '"'


def main():
    source = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=source / "build/linux-release")
    args = parser.parse_args()
    runtime = args.build_dir.resolve()
    if not os.access(runtime / "ObriiSDR", os.X_OK):
        parser.error("Build ObriiSDR before installing its shortcut")
    entry = runtime / "start-obrii.sh"
    if entry.is_file():
        launch = "exec " + shlex.quote(str(entry)) + ' "$@"\n'
    else:
        launch = ("export BUILD_DIR=" + shlex.quote(str(runtime)) + "\n" +
                  "export APP=" + shlex.quote(str(runtime / "ObriiSDR")) + "\n" +
                  "exec bash " + shlex.quote(str(source / "tools/run_linux.sh")) + ' "$@"\n')
    try:
        raspberry_pi = b"Raspberry Pi" in Path("/proc/device-tree/model").read_bytes()
    except OSError:
        raspberry_pi = False
    platform = ""
    if raspberry_pi:
        # Qt 5 Wayland popups in Obrii can miss clicks at fractional scale.
        platform = ('if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -n "${DISPLAY:-}" ]; then\n'
                    '    export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"\n'
                    'fi\n')
    wrapper = runtime / "start-obrii-fusion.sh"
    write_file(wrapper, "#!/bin/sh\nset -eu\nunset QT_QPA_PLATFORMTHEME\n"
               "export QT_STYLE_OVERRIDE=Fusion\nexport QT_SCALE_FACTOR=1.1\n" +
               platform + launch, 0o755)
    desktop = ("[Desktop Entry]\nType=Application\nName=Obrii SDR (Fusion 110%)\n"
               "Name[uk]=Обрій SDR (Fusion 110%)\nComment=Wideband SDR receiver\n"
               "Exec=" + exec_argument(wrapper) + "\n"
               "Path=" + desktop_value(runtime) + "\n"
               "Icon=" + desktop_value(source / "packaging/icons/fobosapp.png") + "\n"
               "Terminal=false\nCategories=AudioVideo;HamRadio;Science;\nStartupNotify=true\n")
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    menu = data / "applications/ObriiSDR-Fusion.desktop"
    write_file(menu, desktop, 0o644)
    print("Installed " + str(menu))
    if shutil.which("xdg-user-dir"):
        result = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True, check=True)
        desktop_dir = Path(result.stdout.strip())
    else:
        desktop_dir = Path.home() / "Desktop"
    if desktop_dir.is_absolute() and desktop_dir != Path.home():
        shortcut = desktop_dir / "ObriiSDR-Fusion.desktop"
        write_file(shortcut, desktop, 0o755)
        if shutil.which("gio"):
            subprocess.run(["gio", "set", str(shortcut), "metadata::trusted", "true"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("Installed " + str(shortcut))


if __name__ == "__main__":
    main()
