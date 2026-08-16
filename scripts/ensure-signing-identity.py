#!/usr/bin/env python3
"""Pick a codesigning identity, or ask Apple to issue Apple Development.

Full Xcode is not required to *build* Drover. Creating a brand-new
certificate does need Apple's signing service. This script:

1. Reuses Apple Development / Developer ID already in the keychain
2. If none exist and Xcode.app is installed, runs a tiny xcodebuild with
   automatic signing so Apple issues a cert into the login keychain
3. Otherwise explains what is missing

Stdout is only the identity hash (or name). Status goes to stderr.
"""

from __future__ import annotations

import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


IDENTITY_RE = re.compile(
    r"^\s*\d+\)\s+([0-9A-F]{40})\s+\"([^\"]+)\"\s*$",
    re.IGNORECASE,
)

PBXPROJ = r"""// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {
	};
	objectVersion = 56;
	objects = {
		A10000000000000000000001 /* main.c in Sources */ = {isa = PBXBuildFile; fileRef = A10000000000000000000002 /* main.c */; };
		A10000000000000000000002 /* main.c */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = main.c; sourceTree = "<group>"; };
		A10000000000000000000003 /* SignProbe */ = {isa = PBXFileReference; explicitFileType = compiled.mach-o.executable; includeInIndex = 0; path = SignProbe; sourceTree = BUILT_PRODUCTS_DIR; };
		A10000000000000000000004 /* Frameworks */ = {isa = PBXFrameworksBuildPhase; buildActionMask = 2147483647; files = (); runOnlyForDeploymentPostprocessing = 0; };
		A10000000000000000000005 = {isa = PBXGroup; children = (A10000000000000000000002 /* main.c */, A10000000000000000000006 /* Products */,); sourceTree = "<group>"; };
		A10000000000000000000006 /* Products */ = {isa = PBXGroup; children = (A10000000000000000000003 /* SignProbe */,); name = Products; sourceTree = "<group>"; };
		A10000000000000000000007 /* SignProbe */ = {
			isa = PBXNativeTarget;
			buildConfigurationList = A10000000000000000000008 /* Build configuration list for PBXNativeTarget "SignProbe" */;
			buildPhases = (A10000000000000000000009 /* Sources */, A10000000000000000000004 /* Frameworks */,);
			buildRules = ();
			dependencies = ();
			name = SignProbe;
			productName = SignProbe;
			productReference = A10000000000000000000003 /* SignProbe */;
			productType = "com.apple.product-type.tool";
		};
		A1000000000000000000000A /* Project object */ = {
			isa = PBXProject;
			attributes = {BuildIndependentTargetsInParallel = 1; LastUpgradeCheck = 1500;};
			buildConfigurationList = A1000000000000000000000B /* Build configuration list for PBXProject "SignProbe" */;
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (en, Base,);
			mainGroup = A10000000000000000000005;
			productRefGroup = A10000000000000000000006 /* Products */;
			projectDirPath = "";
			projectRoot = "";
			targets = (A10000000000000000000007 /* SignProbe */,);
		};
		A10000000000000000000009 /* Sources */ = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (A10000000000000000000001 /* main.c in Sources */,); runOnlyForDeploymentPostprocessing = 0; };
		A1000000000000000000000C /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {ALWAYS_SEARCH_USER_PATHS = NO; CODE_SIGN_IDENTITY = "Apple Development"; CODE_SIGN_STYLE = Automatic; MACOSX_DEPLOYMENT_TARGET = 12.0; SDKROOT = macosx;}; name = Debug;};
		A1000000000000000000000D /* Release */ = {isa = XCBuildConfiguration; buildSettings = {ALWAYS_SEARCH_USER_PATHS = NO; CODE_SIGN_IDENTITY = "Apple Development"; CODE_SIGN_STYLE = Automatic; MACOSX_DEPLOYMENT_TARGET = 12.0; SDKROOT = macosx;}; name = Release;};
		A1000000000000000000000E /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {CODE_SIGN_IDENTITY = "Apple Development"; CODE_SIGN_STYLE = Automatic; DEVELOPMENT_TEAM = "__TEAM__"; MACOSX_DEPLOYMENT_TARGET = 12.0; PRODUCT_BUNDLE_IDENTIFIER = com.discorddrover.signprobe; PRODUCT_NAME = "$(TARGET_NAME)"; SDKROOT = macosx;}; name = Debug;};
		A1000000000000000000000F /* Release */ = {isa = XCBuildConfiguration; buildSettings = {CODE_SIGN_IDENTITY = "Apple Development"; CODE_SIGN_STYLE = Automatic; DEVELOPMENT_TEAM = "__TEAM__"; MACOSX_DEPLOYMENT_TARGET = 12.0; PRODUCT_BUNDLE_IDENTIFIER = com.discorddrover.signprobe; PRODUCT_NAME = "$(TARGET_NAME)"; SDKROOT = macosx;}; name = Release;};
		A1000000000000000000000B /* Build configuration list for PBXProject "SignProbe" */ = {isa = XCConfigurationList; buildConfigurations = (A1000000000000000000000C /* Debug */, A1000000000000000000000D /* Release */,); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release;};
		A10000000000000000000008 /* Build configuration list for PBXNativeTarget "SignProbe" */ = {isa = XCConfigurationList; buildConfigurations = (A1000000000000000000000E /* Debug */, A1000000000000000000000F /* Release */,); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release;};
	};
	rootObject = A1000000000000000000000A /* Project object */;
}
"""


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, text=True, capture_output=True, **kwargs)


def list_identities() -> list[tuple[str, str]]:
    proc = run(["/usr/bin/security", "find-identity", "-v", "-p", "codesigning"])
    found: list[tuple[str, str]] = []
    for line in (proc.stdout or "").splitlines():
        if "CSSMERR" in line:
            continue
        match = IDENTITY_RE.match(line)
        if match:
            found.append((match.group(1).upper(), match.group(2)))
    return found


def pick_identity(identities: list[tuple[str, str]]) -> tuple[str, str] | None:
    def first_named(*needles: str) -> tuple[str, str] | None:
        for sha, name in identities:
            if any(n in name for n in needles):
                return sha, name
        return None

    return (
        first_named("Apple Development")
        or first_named("Mac Developer", "iPhone Developer")
        or first_named("Developer ID Application")
        or first_named("Apple Distribution")
    )


def find_xcode_developer_dir() -> Path | None:
    env = os.environ.get("DEVELOPER_DIR")
    if env:
        path = Path(env)
        if (path / "usr/bin/xcodebuild").is_file():
            return path

    for app in (
        Path("/Applications/Xcode.app"),
        Path("/Applications/Xcode-beta.app"),
    ):
        dev = app / "Contents/Developer"
        if (dev / "usr/bin/xcodebuild").is_file():
            return dev

    proc = run(["/usr/bin/xcode-select", "-p"])
    selected = Path((proc.stdout or "").strip())
    if selected.name != "CommandLineTools" and (selected / "usr/bin/xcodebuild").is_file():
        return selected
    return None


def discover_team_ids() -> list[str]:
    teams: list[str] = []
    env_team = os.environ.get("DEVELOPMENT_TEAM", "").strip()
    if env_team:
        teams.append(env_team)

    roots = [
        Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
        Path.home() / "Library/MobileDevice/Provisioning Profiles",
    ]
    for root in roots:
        if not root.is_dir():
            continue
        for profile in root.glob("*.mobileprovision"):
            decoded = run(["/usr/bin/security", "cms", "-D", "-i", str(profile)])
            if decoded.returncode != 0 or not decoded.stdout:
                continue
            try:
                plist = plistlib.loads(decoded.stdout.encode())
            except Exception:
                continue
            for team in plist.get("TeamIdentifier") or []:
                if isinstance(team, str) and team not in teams:
                    teams.append(team)
    return teams


def write_signprobe(root: Path, team: str) -> Path:
    proj = root / "SignProbe.xcodeproj"
    shutil.rmtree(proj, ignore_errors=True)
    proj.mkdir(parents=True)
    if team:
        pbx = PBXPROJ.replace("__TEAM__", team)
    else:
        pbx = PBXPROJ.replace('DEVELOPMENT_TEAM = "__TEAM__"; ', "")
    (proj / "project.pbxproj").write_text(pbx)
    (root / "main.c").write_text("int main(void) { return 0; }\n")
    return proj


def try_xcodebuild_create() -> str | None:
    developer = find_xcode_developer_dir()
    if developer is None:
        log("No Xcode.app on this Mac — cannot mint a new Apple Development certificate from CLI tools alone.")
        return None

    xcodebuild = developer / "usr/bin/xcodebuild"
    log(f"No signing identity yet. Asking Apple to issue Apple Development via {xcodebuild}...")
    log("This uses the Apple ID already signed into Xcode (Settings → Accounts).")

    teams = discover_team_ids() or [""]
    env = os.environ.copy()
    env["DEVELOPER_DIR"] = str(developer)
    env["NSUnbufferedIO"] = "YES"

    last_err = ""
    with tempfile.TemporaryDirectory(prefix="drover-signprobe-") as tmp:
        tmp_path = Path(tmp)
        for team in teams:
            proj = write_signprobe(tmp_path, team)
            cmd = [
                str(xcodebuild),
                "-project",
                str(proj),
                "-target",
                "SignProbe",
                "-configuration",
                "Debug",
                "-destination",
                "platform=macOS",
                "-allowProvisioningUpdates",
                "-allowProvisioningDeviceRegistration",
                "CODE_SIGN_STYLE=Automatic",
                "CODE_SIGN_IDENTITY=Apple Development",
                "build",
            ]
            if team:
                cmd.append(f"DEVELOPMENT_TEAM={team}")
                log("Trying a development team from an existing profile...")
            proc = subprocess.run(
                cmd,
                cwd=tmp_path,
                text=True,
                capture_output=True,
                env=env,
                timeout=180,
            )
            out = (proc.stdout or "") + "\n" + (proc.stderr or "")
            last_err = out
            if proc.returncode == 0:
                log("Apple issued (or refreshed) a development certificate.")
                return out
            # Keep going through other team IDs.
            shutil.rmtree(proj, ignore_errors=True)

    if "license" in last_err.lower():
        log("Xcode license is not accepted. Run:")
        log("  sudo xcodebuild -license accept")
    elif "no accounts registered" in last_err.lower() or "requires a development team" in last_err.lower():
        log("Xcode has no signed-in Apple ID, so Apple will not issue a certificate.")
        log("Add one once: Xcode → Settings → Accounts → + → Apple ID")
        log("(the same free account from https://developer.apple.com/account/)")
    else:
        log("xcodebuild could not create a certificate.")
    return None


def fail_no_identity() -> None:
    log("No Apple Development (or Developer ID) signing identity found.")
    log("")
    log("A free Apple Developer account is required (the paid $99 program is not).")
    log("  1. Sign in at https://developer.apple.com/account/ and accept the agreement")
    log("  2. Full Xcode is not required to build Drover — only Command Line Tools")
    log("  3. To mint a certificate from the terminal, Xcode.app must be installed")
    log("     and signed into that Apple ID (Settings → Accounts)")
    log("  4. Then re-run ./install.sh — it will request Apple Development automatically")
    log("")
    log("If you already have a .p12 from another Mac:")
    log("  CODESIGN_P12=/path/to/dev.p12 ./install.sh")
    log("")
    log("Ad-hoc signatures crash Discord's renderer.")
    raise SystemExit(1)


def main() -> None:
    preset = os.environ.get("CODESIGN_ID", "").strip()
    if preset:
        log("Using the signing identity from the environment.")
        print(preset)
        return

    identities = list_identities()
    chosen = pick_identity(identities)
    if chosen:
        sha, _name = chosen
        log("Using an existing Apple Development (or Developer ID) identity.")
        print(sha)
        return

    try:
        try_xcodebuild_create()
    except subprocess.TimeoutExpired:
        log("Timed out waiting for Apple to issue a certificate.")
    except OSError as exc:
        log(f"Could not run xcodebuild: {exc}")

    identities = list_identities()
    chosen = pick_identity(identities)
    if chosen:
        sha, _name = chosen
        log("Using a newly issued Apple Development identity.")
        print(sha)
        return

    fail_no_identity()


if __name__ == "__main__":
    main()
