#!/bin/zsh
# Package the already-signed Discord Drover app into a DMG for close friends.
# Uses the same local signing identity as install.sh. Nothing is hardcoded.
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP="${INSTALL_APP:-$HOME/Applications/Discord-Drover.app}"
OUT="${DMG_OUT:-$HOME/Desktop/Discord-Drover.dmg}"
VOL_NAME="Discord Drover"
APP_NAME="Discord-Drover.app"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'EOF'
Usage: ./make-dmg.sh

  Builds a signed DMG of ~/Applications/Discord-Drover.app
  (run ./install.sh first). Default output: ~/Desktop/Discord-Drover.dmg

Environment:
  INSTALL_APP   App to package (default: ~/Applications/Discord-Drover.app)
  DMG_OUT       Output path (default: ~/Desktop/Discord-Drover.dmg)
  CODESIGN_ID   Signing identity (auto-detected, same as install.sh)
EOF
  exit 0
fi

if [[ ! -d "$APP" ]]; then
  echo "No app at $APP"
  echo "Run ./install.sh first."
  exit 1
fi

CODESIGN_ID="$(/usr/bin/python3 "$ROOT/scripts/ensure-signing-identity.py")"
if [[ -z "$CODESIGN_ID" ]]; then
  exit 1
fi

stage="$(/usr/bin/mktemp -d -t drover-dmg)"
cleanup() { /bin/rm -rf "$stage"; }
trap cleanup EXIT

echo "Copying app into the disk image..."
/usr/bin/ditto "$APP" "$stage/$APP_NAME"
/bin/ln -s /Applications "$stage/Applications"
/usr/bin/xattr -cr "$stage/$APP_NAME" >/dev/null 2>&1 || true

echo "Creating $OUT..."
/bin/rm -f "$OUT"
/usr/bin/hdiutil create \
  -volname "$VOL_NAME" \
  -srcfolder "$stage" \
  -ov \
  -format UDZO \
  -imagekey zlib-level=9 \
  "$OUT" >/dev/null

echo "Signing the disk image..."
/usr/bin/codesign --force --sign "$CODESIGN_ID" --timestamp=none "$OUT"

echo
echo "DMG: $OUT"
echo "Friends: open the DMG, drag Discord-Drover into Applications,"
echo "then first launch with right-click → Open (Apple Development is not notarized)."
