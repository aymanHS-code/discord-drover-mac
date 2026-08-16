# Discord Drover for macOS

**Discord Drover** is a second Discord app for Mac that sends a short UDP prelude on each new voice connection so voice can work where it is otherwise blocked. Stock `/Applications/Discord.app` is never modified. The copy lives in `~/Applications` and in the Dock.

On each new voice UDP socket it sends:

1. `drover-packet.bin`
2. a 1-byte `0x00` packet
3. a 1-byte `0x01` packet
4. a 50ms pause
5. Discord’s original packet unchanged

**Before installing:** create a [free Apple Developer account](https://developer.apple.com/account/) (the paid $99 program is not required) so Apple can issue an Apple Development certificate. Full Xcode is not required to build or install.

## Requirements

1. **A free Apple Developer account.** Sign in with any Apple ID at [developer.apple.com/account](https://developer.apple.com/account/) and accept the developer agreement. Without this, Apple will not issue a signing certificate, and install will fail.
2. macOS 12+ (Apple Silicon or Intel)
3. Discord installed at `/Applications/Discord.app`
4. **Xcode Command Line Tools** — `clang`, `make`, `python3`, `codesign`. `./install.sh` checks these and runs `xcode-select --install` if they are missing. The full Xcode app is not required.

Ad-hoc signatures will crash Discord’s renderer. Do not skip signing.

### Signing certificate

`./install.sh` looks in your login keychain and uses an existing **Apple Development** identity if one is already there (Developer ID works too).

If none exists, the script tries to create one from the terminal with `xcodebuild` automatic signing. That step needs:

- the free developer account above
- **Xcode.app installed and signed into that Apple ID** (Settings → Accounts) — only for minting the cert, not for building Drover

If you already have a `.p12` from another Mac:

```bash
CODESIGN_P12=/path/to/dev.p12 ./install.sh
```

## Install

```bash
git clone https://github.com/aymanHS-code/discord-drover-mac.git
cd discord-drover-mac
chmod +x install.sh
./install.sh
```

That command:

- builds `drover_direct.dylib`
- copies Discord to `~/Applications/Discord-Drover.app`
- links Drover into the main app **and** the renderer helper (where `discord_voice.node` lives)
- signs the copy with your Apple Development identity
- adds **Discord Drover** to the Dock
- launches it

Use this Dock icon for voice, not the original Discord.

## Usage

Join a voice channel in **Discord Drover**. Each new channel connection sends the prelude again (including when macOS recycles socket file descriptors).

Logs: `/tmp/discord-drover.log`

## Updating

After a Discord desktop update:

```bash
./install.sh --refresh
```

After changing Drover source:

```bash
./run-discord.sh
```

## Share with friends

After `./install.sh`, build a signed disk image (default: `~/Desktop/Discord-Drover.dmg`):

```bash
chmod +x make-dmg.sh
./make-dmg.sh
```

Friends drag **Discord-Drover** into Applications. The first launch needs **right-click → Open** because this uses an Apple Development certificate, not a notarized Developer ID.

## Notes

- `./install.sh` launches with `--disable-gpu` so the window is not black after signing.
- Electron Framework is left on Discord’s original Developer ID.
- `DYLD_INSERT_LIBRARIES` is not used; it crashes Chromium helpers.

## Credits

Windows Discord Drover (including Direct mode) was written by **hdrover**: [https://github.com/hdrover/discord-drover](https://github.com/hdrover/discord-drover).

## License / disclaimer

This wraps Discord for your own machine. It is not affiliated with Discord or with the original Windows project. Use it only where you are allowed to.
