#!/usr/bin/env bash
# Launch the Debug build with the runtime environment it needs on NixOS
# (WebKitGTK for WebApps/Mini Apps, Qt image plugins, NVIDIA EGL wiring).
# Environment lives in flake.nix shellHook; this script just applies it.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec nix develop "$repo" -c "$repo/out/Debug/Telegram" "$@"
