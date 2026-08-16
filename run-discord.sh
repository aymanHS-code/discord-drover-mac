#!/bin/zsh
# Rebuild/update the installed Discord Drover copy and launch it.
# First-time setup: ./install.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
exec "$ROOT/install.sh" --no-dock "$@"
