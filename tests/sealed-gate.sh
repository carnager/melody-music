#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# The test suite sealed off from the session it runs in: a network of its
# own (only its loopback, multicast on it), a session bus of its own, and the
# user's runtime directory hidden -- no test can reach the user's engines,
# desktop or speakers, not even by a socket's path. A test once named
# /run/user/1000/melodyd.sock and handed the user's engine its settings.
#   tests/sealed-gate.sh BUILD-DIR [ctest arguments...]
set -euo pipefail
build="$1"; shift
none="$(mktemp -d)"
trap 'rm -rf "${none}"' EXIT
exec unshare -rnm dbus-run-session -- sh -c '
    # Where the engine and desktop of the user keep their sockets: empty
    # here but for PipeWire, which the playback tests play silence through.
    # (The runtime directory of the tests is in the build tree.) No
    # apostrophes in this script: it is quoted in them.
    if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "${XDG_RUNTIME_DIR}" ]; then
        real="$(mktemp -d)"
        mount --rbind "${XDG_RUNTIME_DIR}" "${real}"
        mount -t tmpfs none "${XDG_RUNTIME_DIR}"
        for socket in pipewire-0 pipewire-0-manager; do
            if [ -S "${real}/${socket}" ]; then
                : > "${XDG_RUNTIME_DIR}/${socket}"
                mount --bind "${real}/${socket}" "${XDG_RUNTIME_DIR}/${socket}"
            fi
        done
        umount -l "${real}"
    fi
    ip link set lo up
    # A network of its own to find engines on: multicast DNS needs an
    # interface that is not loopback.
    ip link add sealed0 type dummy
    ip addr add 10.213.0.1/24 dev sealed0
    ip link set sealed0 multicast on up
    ip route add 224.0.0.0/4 dev sealed0
    export PIPEWIRE_RUNTIME_DIR="$1" PULSE_SERVER=unix:/nonexistent
    shift
    exec ctest --test-dir "$@"
' sealed "${none}" "${build}" "$@"
