#!/bin/zsh
# One-shot installer: build Drover, wrap a copy of Discord, sign it, put it
# in ~/Applications, pin it to the Dock, and launch it.
set -euo pipefail
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"

ROOT="$(cd "$(dirname "$0")" && pwd)"
DYLIB="$ROOT/drover_direct.dylib"
STUB_SRC="$ROOT/discord_stub.c"
PACKET="$ROOT/drover-packet.bin"
ENTITLEMENTS="$ROOT/entitlements.plist"
ENTITLEMENTS_RENDERER="$ROOT/entitlements-renderer.plist"
ENTITLEMENTS_CHILD="$ROOT/entitlements-child.plist"
ENTITLEMENTS_GPU="$ROOT/entitlements-gpu.plist"
SOURCE_APP="${DISCORD_APP:-/Applications/Discord.app}"
INSTALL_APP="${INSTALL_APP:-$HOME/Applications/Discord-Drover.app}"
RENDERER_HELPER="$INSTALL_APP/Contents/Frameworks/Discord Helper (Renderer).app"
DROVER_LOG="${DROVER_LOG:-/tmp/discord-drover.log}"
BUNDLE_ID="com.hnc.Discord"
DISPLAY_NAME="Discord Drover"

refresh=0
add_dock=1
launch=1
for arg in "$@"; do
  case "$arg" in
    --refresh) refresh=1 ;;
    --no-dock) add_dock=0 ;;
    --no-launch) launch=0 ;;
    --help|-h)
      cat <<'EOF'
Usage: ./install.sh [--refresh] [--no-dock] [--no-launch]

  Copies Discord, injects Drover, signs the copy, adds it to the Dock,
  and launches it. Full Xcode is not required.

  --refresh     Recopy from /Applications/Discord.app even if a copy exists
  --no-dock     Do not add the app to the Dock
  --no-launch   Do not launch after installing

Environment:
  DISCORD_APP       Source Discord (default: /Applications/Discord.app)
  INSTALL_APP       Destination (default: ~/Applications/Discord-Drover.app)
  CODESIGN_ID       Signing identity hash or name (auto-detected)
  CODESIGN_P12      Optional .p12 to import before signing
  DEVELOPMENT_TEAM  Team ID used only when minting a new certificate
EOF
      exit 0
      ;;
    *)
      echo "Unknown option: $arg (try --help)"
      exit 1
      ;;
  esac
done

clang_works() {
  local tmp out
  tmp="$(/usr/bin/mktemp -t drover-clt)"
  out="${tmp}.out"
  /bin/rm -f "$tmp"
  if /usr/bin/clang -x c -o "$out" - >/dev/null 2>&1 <<'EOF'
int main(void) { return 0; }
EOF
  then
    /bin/rm -f "$out"
    return 0
  fi
  /bin/rm -f "$out"
  return 1
}

tool_ok() {
  local bin="$1"
  if [[ ! -x "$bin" ]]; then
    return 1
  fi
  # /usr/bin/clang and /usr/bin/python3 are stubs until CLT or Xcode exists.
  case "$bin" in
    */clang)
      clang_works
      ;;
    */python3)
      "$bin" -c 'import sys' >/dev/null 2>&1
      ;;
    */make)
      "$bin" --version >/dev/null 2>&1
      ;;
    */otool)
      "$bin" -hv /usr/bin/true >/dev/null 2>&1
      ;;
    */install_name_tool)
      local out
      out="$("$bin" 2>&1 || true)"
      [[ "$out" == *'install_name_tool'* && "$out" != *'xcode-select'* ]]
      ;;
    *)
      return 0
      ;;
  esac
}

ensure_cli_tools() {
  local needed=(
    /usr/bin/clang
    /usr/bin/make
    /usr/bin/python3
    /usr/bin/codesign
    /usr/bin/otool
    /usr/bin/install_name_tool
    /usr/bin/ditto
    /usr/bin/security
    /usr/libexec/PlistBuddy
  )
  local missing=()
  local bin elapsed

  for bin in "${needed[@]}"; do
    if ! tool_ok "$bin"; then
      missing+=("$bin")
    fi
  done
  if (( ${#missing[@]} == 0 )); then
    return
  fi

  echo "Missing developer tools: ${missing[*]}"
  echo "Installing Xcode Command Line Tools (the full Xcode app is not required)..."
  echo "If a macOS dialog appears, click Install and wait."
  /usr/bin/xcode-select --install >/dev/null 2>&1 || true

  elapsed=0
  while (( elapsed < 900 )); do
    missing=()
    for bin in "${needed[@]}"; do
      if ! tool_ok "$bin"; then
        missing+=("$bin")
      fi
    done
    if (( ${#missing[@]} == 0 )); then
      echo "Command Line Tools are ready."
      return
    fi
    /bin/sleep 4
    elapsed=$((elapsed + 4))
    if (( elapsed % 20 == 0 )); then
      echo "  still waiting (${elapsed}s) — finish the Install dialog if it is open"
    fi
  done

  echo "Timed out waiting for Command Line Tools."
  echo "Install them with:  xcode-select --install"
  echo "Or from: https://developer.apple.com/download/all/?q=command%20line"
  exit 1
}

import_codesign_p12() {
  if [[ -z "${CODESIGN_P12:-}" ]]; then
    return
  fi
  if [[ ! -f "$CODESIGN_P12" ]]; then
    echo "CODESIGN_P12 is set but not a file: $CODESIGN_P12"
    exit 1
  fi
  echo "Importing $CODESIGN_P12 into the login keychain..."
  if ! /usr/bin/security import "$CODESIGN_P12" \
    -k "$HOME/Library/Keychains/login.keychain-db" \
    -P "${CODESIGN_P12_PASSWORD:-}" \
    -T /usr/bin/codesign \
    -T /usr/bin/security >/dev/null; then
    echo "Note: import reported an error (the identity may already be in the keychain)."
  fi
}

ensure_cli_tools
import_codesign_p12

if [[ ! -d "$SOURCE_APP" ]]; then
  echo "Discord.app was not found at $SOURCE_APP"
  echo "Install Discord from https://discord.com/download first."
  exit 1
fi

if [[ ! -f "$PACKET" ]]; then
  echo "Missing $PACKET"
  echo "drover-packet.bin must sit next to install.sh."
  exit 1
fi

pick_codesign_id() {
  local id
  id="$(/usr/bin/python3 "$ROOT/scripts/ensure-signing-identity.py")"
  if [[ -z "$id" ]]; then
    exit 1
  fi
  CODESIGN_ID="$id"
}

links_drover() {
  local binary="$1"
  /usr/bin/python3 - "$binary" <<'PY'
import shutil, subprocess, sys, tempfile
from pathlib import Path
src = Path(sys.argv[1])
tmp = Path(tempfile.mkstemp()[1])
try:
    shutil.copy2(src, tmp)
    out = subprocess.check_output(["/usr/bin/otool", "-L", str(tmp)], text=True)
    sys.exit(0 if "drover_direct.dylib" in out else 1)
finally:
    tmp.unlink(missing_ok=True)
PY
}

renderer_signed_ok() {
  local helper="$1"
  [[ -d "$helper" ]] || return 1
  local info ents
  info="$(/usr/bin/codesign -dv "$helper" 2>&1)" || return 1
  echo "$info" | /usr/bin/grep -q "runtime" || return 1
  echo "$info" | /usr/bin/grep -q "Signature=adhoc" && return 1
  ents="$(/usr/bin/codesign -d --entitlements :- "$helper" 2>/dev/null)" || return 1
  echo "$ents" | /usr/bin/grep -q "allow-jit" || return 1
  if echo "$ents" | /usr/bin/grep -q "allow-dyld-environment-variables"; then
    return 1
  fi
  return 0
}

sign_id() {
  local target="$1"
  shift
  local ents=""
  if [[ "${1:-}" == "--entitlements" ]]; then
    ents="$2"
  fi
  /usr/bin/python3 "$ROOT/scripts/codesign-id.py" "$CODESIGN_ID" "$target" "$ents"
}

sign_nested() {
  local app="$1"
  local items=()
  local path ents

  echo "Signing nested binaries (leaving Electron Framework on Discord's original signature)..."

  while IFS= read -r path; do
    items+=("$path")
  done < <(
    {
      /usr/bin/find "$app/Contents" -type f \( -name "*.dylib" -o -name "*.so" -o -name "*.node" \)
      /usr/bin/find "$app/Contents" -type d \( -name "*.framework" -o -name "*.app" \)
      /usr/bin/find "$app/Contents/MacOS" -type f
    } | /usr/bin/grep -v "Electron Framework.framework" | /usr/bin/awk '{ print length, $0 }' | /usr/bin/sort -nr | /usr/bin/cut -d' ' -f2-
  )

  for path in "${items[@]}"; do
    ents=()
    case "$path" in
      *"Helper (Renderer).app"|*"Helper (Renderer)")
        ents=(--entitlements "$ENTITLEMENTS_RENDERER")
        ;;
      *"Helper (GPU).app"|*"Helper (GPU)"|*"Helper (Plugin).app"|*"Helper (Plugin)")
        ents=(--entitlements "$ENTITLEMENTS_GPU")
        ;;
      *"Helper.app"|*/"Discord Helper")
        ents=(--entitlements "$ENTITLEMENTS_CHILD")
        ;;
      */Contents/MacOS/Discord)
        ents=(--entitlements "$ENTITLEMENTS")
        ;;
    esac
    sign_id "$path" "${ents[@]}"
  done

  sign_id "$app" --entitlements "$ENTITLEMENTS"
}

brand_app() {
  local plist="$1/Contents/Info.plist"
  # Keep CFBundleName / identifier as Discord so Electron can find helpers.
  /usr/libexec/PlistBuddy -c "Set :CFBundleName Discord" "$plist"
  /usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName Discord Drover" "$plist"
  /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.hnc.Discord" "$plist"
}

install_stub() {
  local app="$1"
  local frameworks="$app/Contents/Frameworks"
  local dest_dylib="$frameworks/drover_direct.dylib"
  local renderer_bin="$app/Contents/Frameworks/Discord Helper (Renderer).app/Contents/MacOS/Discord Helper (Renderer)"

  echo "Installing Drover into the Discord copy..."
  /bin/cp "$DYLIB" "$dest_dylib"
  /usr/bin/install_name_tool -id @rpath/drover_direct.dylib "$dest_dylib"

  if [[ ! -f "$PACKET" ]]; then
    echo "Missing $PACKET"
    echo "drover-packet.bin must sit next to install.sh."
    exit 1
  fi
  echo "Copying drover-packet.bin into Contents/Resources..."
  /bin/mkdir -p "$app/Contents/Resources"
  /bin/cp "$PACKET" "$app/Contents/Resources/drover-packet.bin"

  /usr/bin/clang -arch arm64 -arch x86_64 -O2 \
    -o "$app/Contents/MacOS/Discord" \
    "$STUB_SRC" \
    "$dest_dylib" \
    -F "$frameworks" \
    -framework "Electron Framework" \
    -Wl,-rpath,@executable_path/../Frameworks

  /usr/bin/clang -arch arm64 -arch x86_64 -O2 \
    -o "$renderer_bin" \
    "$STUB_SRC" \
    "$dest_dylib" \
    -F "$frameworks" \
    -framework "Electron Framework" \
    -Wl,-rpath,@executable_path/../../../
}

pick_codesign_id
echo "Signing identity selected."

echo "Building drover_direct.dylib..."
/usr/bin/make -C "$ROOT" dylib

/usr/bin/osascript -e 'quit app "Discord Drover"' >/dev/null 2>&1 || true
/usr/bin/osascript -e 'quit app "Discord"' >/dev/null 2>&1 || true
/usr/bin/pkill -x Discord >/dev/null 2>&1 || true
/bin/sleep 1

need_copy=0
if [[ ! -d "$INSTALL_APP" || "$refresh" -eq 1 ]]; then
  need_copy=1
elif ! renderer_signed_ok "$RENDERER_HELPER"; then
  echo "Existing copy is not signed correctly; recopying."
  need_copy=1
elif ! links_drover "$INSTALL_APP/Contents/MacOS/Discord"; then
  echo "Existing copy is missing the Drover stub; recopying."
  need_copy=1
fi

if [[ "$need_copy" -eq 1 ]]; then
  echo "Copying Discord to $INSTALL_APP"
  /bin/mkdir -p "$(dirname "$INSTALL_APP")"
  /bin/rm -rf "$INSTALL_APP"
  /usr/bin/ditto "$SOURCE_APP" "$INSTALL_APP"
  /usr/bin/xattr -d com.apple.quarantine "$INSTALL_APP" >/dev/null 2>&1 || true
  brand_app "$INSTALL_APP"
  install_stub "$INSTALL_APP"
  sign_nested "$INSTALL_APP"
else
  echo "Updating Drover in $INSTALL_APP"
  brand_app "$INSTALL_APP"
  install_stub "$INSTALL_APP"
  sign_id "$INSTALL_APP/Contents/Frameworks/drover_direct.dylib"
  sign_id "$RENDERER_HELPER" --entitlements "$ENTITLEMENTS_RENDERER"
  sign_id "$INSTALL_APP/Contents/MacOS/Discord" --entitlements "$ENTITLEMENTS"
  sign_id "$INSTALL_APP" --entitlements "$ENTITLEMENTS"
fi

if ! renderer_signed_ok "$RENDERER_HELPER"; then
  echo "Renderer helper is missing JIT entitlements; aborting."
  /usr/bin/codesign -dvvv --entitlements :- "$RENDERER_HELPER" || true
  exit 1
fi
if ! links_drover "$INSTALL_APP/Contents/MacOS/Discord"; then
  echo "Discord stub is not linked to drover_direct.dylib; aborting."
  exit 1
fi
if ! links_drover "$RENDERER_HELPER/Contents/MacOS/Discord Helper (Renderer)"; then
  echo "Renderer helper is not linked to drover_direct.dylib; aborting."
  exit 1
fi

# Previous installer used a space in the app name; Electron aborts on that path.
old_spaced="$HOME/Applications/Discord Drover.app"
if [[ -d "$old_spaced" && "$old_spaced" != "$INSTALL_APP" ]]; then
  echo "Removing old $old_spaced (space in the name crashed Electron)"
  /bin/rm -rf "$old_spaced"
fi

if [[ "$add_dock" -eq 1 ]]; then
  echo "Adding $DISPLAY_NAME to the Dock..."
  dock_result="$(/usr/bin/python3 "$ROOT/scripts/add-to-dock.py" "$INSTALL_APP")"
  if [[ "$dock_result" == "already" ]]; then
    echo "Already in the Dock."
  else
    echo "Dock icon added."
  fi
fi

echo
echo "Installed: $INSTALL_APP"
echo "Log:       $DROVER_LOG"

if [[ "$launch" -eq 1 ]]; then
  : > "$DROVER_LOG"
  echo "Launching $DISPLAY_NAME..."
  /usr/bin/open -n \
    --env "DROVER_DEBUG=${DROVER_DEBUG:-0}" \
    "$INSTALL_APP" \
    --args --disable-gpu
fi
