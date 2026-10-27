#!/bin/bash
# One attempt at the runtime scene.load crash (TODO.md, 2026-08-25).
#
# Usage: Tools/Debug/repro-scene-load-crash.sh [Release|Debug]
#
# The crash is intermittent, so run this several times. It prints DIED or SURVIVED
# and copies the log aside when it dies.
#
# Two things this guards that cost real debugging time:
#   - It refuses to run unless exactly one PBRApp is alive. Two instances share the
#     GPU and steal each other's debug pipe, which reads as random timings and
#     random "the app died on its own".
#   - It enables the cluster path first. Without render.meshshader 1 the reload does
#     not rebuild the cluster DAG and the crash does not appear at all.
set -u
CONFIG="${1:-Release}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO" || exit 1

EXE_DIR="Build/bin/x64/${CONFIG}"
LOG="${EXE_DIR}/PBRApp.exe.log"
PIPE="powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Debug/pipe.ps1"

count() { powershell.exe -NoProfile -Command "@(Get-Process PBRApp -ErrorAction SilentlyContinue).Count" 2>&1 | tr -d '\r'; }

for _ in $(seq 1 8); do
    [ "$(count)" = "0" ] && break
    powershell.exe -NoProfile -Command "Get-Process PBRApp -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
done
if [ "$(count)" != "0" ]; then
    echo "ABORT: PBRApp instances remain; a measurement now would be meaningless."
    exit 1
fi

# Use the repo's own launcher: it sets SASAMI_DEBUG_REMOTE=1, without which there is no
# debug pipe to drive, and it detaches the process so it outlives this shell.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Debug/run-wsl.ps1 -Configuration "$CONFIG" >/dev/null 2>&1

for _ in $(seq 1 150); do
    case "$($PIPE "camera.get" 2>&1)" in *target*) break;; esac
done
if [ "$(count)" != "1" ]; then
    echo "ABORT: expected exactly one instance after launch."
    exit 1
fi

$PIPE "render.meshshader 1" >/dev/null 2>&1      # without this the DAG is never rebuilt
for _ in $(seq 1 8); do $PIPE "render.gputime" >/dev/null 2>&1; done
$PIPE "scene.load PBRApp.scene" >/dev/null 2>&1  # may answer ERR timeout; the reload still runs

for _ in $(seq 1 25); do
    if [ "$(count)" = "0" ]; then
        SAVED="${EXE_DIR}/crash-$(date +%Y%m%d-%H%M%S).log"
        cp "$LOG" "$SAVED" 2>/dev/null
        echo "DIED (log saved to ${SAVED})"
        exit 0
    fi
done
echo "SURVIVED"
