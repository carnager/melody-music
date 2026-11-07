# ADR-0349: Optional engine status icon

## Status

Accepted, 2026-11-07.

## Context

An engine can keep running with no Melody window open. A desktop user needs
to see whether it is responding, restart its service, and rescan its library
without opening the full music client. macOS uses a menu-bar status item;
Linux desktops expose a system tray, when their shell supports one.

## Decision

`melody-status` is an optional Qt companion, not a GUI dependency of melodyd.
It polls the engine's trusted local Unix socket off the GUI thread. The menu
shows running/unavailable status, Rescan Library, Restart melodyd, and Quit
Status Icon. The icon remains visible but dimmed when the engine is unavailable;
quitting it leaves the engine and an already submitted scan running.

Rescan uses the existing `catalogue.scan` job, with progress and an explicit
error/incomplete outcome. It does not read the database or scan files itself.
Restart goes through the local service manager: `launchctl kickstart -k` for
an explicitly named GUI LaunchAgent on macOS, `systemctl --user restart` for
the named user unit (default melodyd.service) on Linux. There is no arbitrary
shell command. An unmanaged engine is not silently replaced by another
process; the Mac restart command is unavailable without a service label.

`status-icon = yes` / `--status-icon` asks melodyd to start the companion on
its own socket; `status-service` / `--status-service` names the service. Both
are optional; the engine stays headless by default and still starts if the
companion is missing or no tray is available. `MELODY_BUILD_STATUS_ICON`
controls the Qt companion build, independently of the music window. A
per-socket lock prevents duplicate icons across restarts. A separate desktop
login item is also supported and can show an unavailable engine before it
starts. That is preferable for services started outside a graphical login.

Service managers may restart a daemon-spawned icon with the daemon's process
group. A separately registered desktop login item stays up across that restart.

The Mac icon is a template: a monochrome version of Melody's existing vinyl
bird SVG, with the eye and record grooves cut out. The system chooses its
menu-bar color. Linux uses the existing color icon. No new artwork dependency
or third-party tray library is introduced.

## Validation

Tests cover running/unavailable/reconnected state, submitting exactly one
scan, scan success and incomplete results, exiting during a scan, icon
transparency/dimming, service argument construction, and daemon config/flag
precedence. Native service and desktop checks are recorded with the change.

Native macOS validation used a temporary launchd engine service: Rescan
indexed its one-file fixture library and Restart replaced its process while
the separately started icon remained running. The companion tests passed on
both the development Mac and a second Mac. Linux service arguments are tested;
a Linux desktop runtime has not been exercised.
