#!/usr/bin/env python3
"""Replace gemenon's SSHFS automount with writable NFS, matching the desktop.

Preview: python3 setup-gemenon-nfs.py
Apply:   sudo python3 setup-gemenon-nfs.py --apply

Requires the TrueNAS export to allow gemenon and map client access to carnager.
Checks read/write access as carnager before stopping the existing mount. The
check creates and removes one temporary file in Music, never in the share root.
"""

import argparse
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import sys
import tempfile
import time


SOURCE = "tauron:/mnt/MEDIA/MEDIA"
TARGET = "/mnt/nas"
SERVICE_USER = "carnager"
UNITS = ("mnt-nas.mount", "mnt-nas.automount")
UNIT_DIR = Path("/etc/systemd/system")
MOUNT_UNIT = f"""[Unit]
Description=NFS mount of tauron media
After=network-online.target
Wants=network-online.target

[Mount]
What={SOURCE}
Where={TARGET}
Type=nfs4
Options=rw,noatime,_netdev,hard
TimeoutSec=90

[Install]
WantedBy=multi-user.target
"""
AUTOMOUNT_UNIT = f"""[Unit]
Description=Automount NFS tauron media

[Automount]
Where={TARGET}
TimeoutIdleSec=600

[Install]
WantedBy=multi-user.target
"""


def run(*args, check=True, capture=False, limit=None):
    return subprocess.run(
        args, check=check, text=True, capture_output=capture, timeout=limit
    )


def user_systemctl(*args, check=True, capture=False):
    uid = pwd.getpwnam(SERVICE_USER).pw_uid
    return run(
        "runuser", "-u", SERVICE_USER, "--", "env",
        f"XDG_RUNTIME_DIR=/run/user/{uid}",
        f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{uid}/bus",
        "systemctl", "--user", *args, check=check, capture=capture,
    )


def write_probe(root):
    # A successful write in this owner-writable directory also checks the NAS's
    # Mapall/permissions, unlike the world-writable flac directory beneath it.
    code = """
import os, pathlib, sys, tempfile
root = pathlib.Path(sys.argv[1])
with os.scandir(root / 'Music/Rips/flac') as entries:
    next(entries, None)
fd, path = tempfile.mkstemp(prefix='.melody-nfs-write-test-', dir=root / 'Music')
try:
    with os.fdopen(fd, 'wb') as f:
        f.write(b'Melody NFS write test\\n')
        f.flush()
        os.fsync(f.fileno())
        st = os.fstat(f.fileno())
    # Creating a file can succeed even when inherited ACLs make the resulting
    # file read-only. Reopen it to test permissions on the created inode too.
    with open(path, 'ab') as f:
        f.write(b'Reopen permission test\\n')
        f.flush()
        os.fsync(f.fileno())
    os.utime(path, None)
    print(f'NFS read/write check passed; server ownership {st.st_uid}:{st.st_gid}')
finally:
    os.unlink(path)
"""
    run(
        "timeout", "--foreground", "--kill-after=5s", "30s",
        "runuser", "-u", SERVICE_USER, "--", "python3", "-c", code, str(root),
    )


def check_nfs_kernel():
    def registered():
        return any(
            line.split()[-1] == "nfs4"
            for line in Path("/proc/filesystems").read_text().splitlines()
            if line.split()
        )

    if registered():
        return
    kernel = os.uname().release
    if not Path(f"/usr/lib/modules/{kernel}").is_dir():
        raise RuntimeError(
            f"Running kernel {kernel} has no matching modules installed. "
            "Reboot gemenon into the installed kernel, then rerun this script. "
            "The existing SSHFS setup has not been changed."
        )
    run("modprobe", "fs-nfs4")
    if not registered():
        raise RuntimeError("The running kernel did not register NFSv4 after loading its module.")


def preflight():
    if os.geteuid() != 0:
        raise RuntimeError("Run --apply as root.")
    if socket.gethostname().split(".")[0] != "gemenon":
        raise RuntimeError("This script is intended for gemenon only.")
    os.chdir("/")
    for name in ("systemctl", "systemd-analyze", "findmnt", "mount", "umount", "modprobe", "runuser", "timeout"):
        if not shutil.which(name):
            raise RuntimeError(f"Missing required command: {name}")
    check_nfs_kernel()
    pwd.getpwnam(SERVICE_USER)
    socket.getaddrinfo("tauron", 2049)
    for line in Path("/etc/fstab").read_text().splitlines():
        fields = line.split()
        if fields and not fields[0].startswith("#") and len(fields) > 1 and fields[1] == TARGET:
            raise RuntimeError("Remove the conflicting /mnt/nas fstab entry before applying.")
    for unit in UNITS:
        path = UNIT_DIR / unit
        if not path.is_file() or path.is_symlink():
            raise RuntimeError(f"Expected a regular existing unit file: {path}")
        dropins = run("systemctl", "show", unit, "-p", "DropInPaths", "--value", capture=True).stdout.strip()
        if dropins:
            raise RuntimeError(f"Review existing drop-ins before applying: {dropins}")
    if not (shutil.which("mount.nfs4") or shutil.which("mount.nfs")):
        if not shutil.which("pacman"):
            raise RuntimeError("Install the NFS client tools before applying.")
        print("Installing the missing Arch Linux NFS client tools.", flush=True)
        run("pacman", "-S", "--needed", "nfs-utils")

    # Keep SSHFS and melodyd running until the alternative has passed its check.
    probe = Path(tempfile.mkdtemp(prefix="melody-nfs-probe-", dir="/run"))
    probe.chmod(0o755)
    try:
        run(
            "timeout", "--foreground", "--kill-after=5s", "45s",
            "mount", "-t", "nfs4", "-o", "rw,noatime,hard,retry=0", SOURCE, str(probe),
        )
        write_probe(probe)
    finally:
        mounted = run("findmnt", "--mountpoint", str(probe), "--noheadings", check=False, capture=True)
        if mounted.returncode == 0:
            run("umount", str(probe))
        probe.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="install and activate the NFS automount")
    args = parser.parse_args()
    if not args.apply:
        print(f"Proposed {UNIT_DIR / UNITS[0]}:\n{MOUNT_UNIT}")
        print(f"Proposed {UNIT_DIR / UNITS[1]}:\n{AUTOMOUNT_UNIT}")
        print("With --apply: install nfs-utils if missing; test NFS read/write as carnager;")
        print("back up both units; stop melodyd; replace SSHFS; enable only the automount;")
        print("verify access; restart melodyd if it was running. Restore old units on failure.")
        print("The existing melodyd required_mounts = [\"/mnt/nas\"] setting remains valid.")
        return

    preflight()
    enabled = {}
    active = {}
    for unit in UNITS:
        state = run("systemctl", "is-enabled", unit, check=False, capture=True).stdout.strip()
        if state not in ("enabled", "disabled"):
            raise RuntimeError(f"Unexpected enablement for {unit}: {state}")
        enabled[unit] = state == "enabled"
        active[unit] = run("systemctl", "is-active", "--quiet", unit, check=False).returncode == 0
    manager = Path(f"/run/user/{pwd.getpwnam(SERVICE_USER).pw_uid}/bus")
    melody_running = manager.exists() and user_systemctl(
        "is-active", "--quiet", "melodyd.service", check=False, capture=True
    ).returncode == 0
    backup = Path(tempfile.mkdtemp(prefix="melody-nfs-" + time.strftime("%Y%m%d-%H%M%S") + "-", dir="/var/tmp"))
    for unit in UNITS:
        shutil.copy2(UNIT_DIR / unit, backup / unit)
    (backup / "previous-state.txt").write_text(
        f"enabled={enabled}\nactive={active}\nmelody_running={melody_running}\n"
    )
    print(f"Original unit files and state saved in {backup}", flush=True)

    with tempfile.TemporaryDirectory(prefix="melody-nfs-units-", dir="/run") as staging:
        for name, content in zip(UNITS, (MOUNT_UNIT, AUTOMOUNT_UNIT)):
            (Path(staging) / name).write_text(content)
        run("systemd-analyze", "verify", *(str(Path(staging) / name) for name in UNITS))
        changed = False
        try:
            if melody_running:
                user_systemctl("stop", "melodyd.service")
            # No lazy/forced unmount: if another process is using the share,
            # leave the old unit files intact and report the failure.
            run("systemctl", "stop", "mnt-nas.automount", "mnt-nas.mount")
            changed = True
            run("systemctl", "disable", *UNITS)
            for name in UNITS:
                run("install", "-m", "644", str(Path(staging) / name), str(UNIT_DIR / name))
            run("systemctl", "daemon-reload")
            run("systemctl", "reset-failed", *UNITS, check=False)
            run("systemctl", "enable", "--now", "mnt-nas.automount")
            write_probe(Path(TARGET))
            fs = run("findmnt", "--mountpoint", TARGET, "--types", "nfs4,nfs", "--noheadings", "--output", "FSTYPE,OPTIONS", capture=True).stdout
            if not fs.strip() or "rw" not in fs.split(None, 1)[1].strip().split(","):
                raise RuntimeError(f"Expected a writable NFS mount, got: {fs}")
            if melody_running:
                user_systemctl("start", "melodyd.service")
                user_systemctl("is-active", "--quiet", "melodyd.service")
        except BaseException:
            print(f"Setup failed; restoring the previous setup from {backup}.", file=sys.stderr)
            if melody_running:
                user_systemctl("stop", "melodyd.service", check=False)
            if changed:
                # Refuse to overwrite active units if NFS cannot be unmounted.
                run("systemctl", "stop", "mnt-nas.automount", "mnt-nas.mount")
                run("systemctl", "disable", *UNITS)
                for name in UNITS:
                    shutil.copy2(backup / name, UNIT_DIR / name)
                run("systemctl", "daemon-reload")
                for name in UNITS:
                    if enabled[name]:
                        run("systemctl", "enable", name)
            for name in reversed(UNITS):
                if active[name]:
                    run("systemctl", "start", name)
            if melody_running:
                user_systemctl("start", "melodyd.service")
            raise
    run("findmnt", "--target", TARGET, "--submounts", "--output", "TARGET,SOURCE,FSTYPE,OPTIONS")
    print("Done: writable NFS, on-demand mount, 600-second idle expiry.")
    print(f"Backup: {backup}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
