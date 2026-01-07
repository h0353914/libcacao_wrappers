#!/usr/bin/env python3

import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

adb = "/mnt/f/Android/platform-tools/adb.exe"

CAMERA_PKG = "com.sonyericsson.android.camera"
CAMERA_ACT = "com.sonyericsson.android.camera/.CameraActivity"

EXTRA_CAMERA_MODE = "com.google.assistant.extra.CAMERA_MODE"
EXTRA_CAMERA_OPEN_ONLY = "com.google.assistant.extra.CAMERA_OPEN_ONLY"

MODE_PATTERN = re.compile(r"SLOW_MOTION|SUPER_SLOW_MOTION", re.IGNORECASE)
FR_960_PATTERN = re.compile(r"framerate:960", re.IGNORECASE)


def enable_super_slow_motion_gate_best_effort() -> bool:
        """Best-effort: make PlatformCapability.isSuperSlowMotionSupported() return true.

        Dex analysis shows the gate checks whether CameraCapabilityList.SUPER_SLOW_VALUES
        contains "on". That value comes from SharedPreferences key "super-slow-values"
        stored in per-camera prefs named:
            com.sonyericsson.android.camera.supported_values.<cameraDeviceId>

        The string-list encoding is ';' separated and the first token must be a
        positive integer (e.g. "1;on").
        """

        host_tmp = "/tmp/enable_super_slow_gate.sh"
        dev_tmp = "/data/local/tmp/enable_super_slow_gate.sh"
        desired = "1;on"
        # VideoConfigurationList encoding (SharedPrefsTranslator.getVideoConfigurationList):
        #   "<count>;<w>x<h>@<frameNum>/<fps>[;<w>x<h>@<frameNum>/<fps>...]"
        # VideoConfiguration.parse details (VideoConfiguration.parse):
        #   - entry delimiter: ';' (when called from SharedPrefsTranslator)
        #   - entry format: "<w>x<h>[@<frameNum>]/<fps>"
        # If '@<frameNum>' is omitted, frameNum becomes 0.
        # Include both 720p and 1080p entries; the app may match on different video sizes.
        super_slow_config = "2;1280x720@192/960;1920x1080@192/960"
        capability_version = 1

        # If the camera app hasn't generated supported_values.* yet, creating a minimal
        # file can cause many unrelated capabilities to appear unsupported. Prefer a
        # short warm-up launch to let the app generate its own cache first.
        probe = adb_shell(
            "su -c 'PKG=com.sonyericsson.android.camera; D=/data/user/0/$PKG/shared_prefs; "
            "[ -d $D ] || D=/data/data/$PKG/shared_prefs; "
            "ls -1 $D/com.sonyericsson.android.camera.supported_values.*.xml 2>/dev/null | head -n 1'",
            check=False,
        )
        if (probe.stdout or "").strip() == "":
            run(f"{adb} shell am start -W -n {CAMERA_ACT}", check=False)
            time.sleep(5)
            adb_shell(f"am force-stop {CAMERA_PKG}", check=False)
            time.sleep(1)

        script = f"""#!/system/bin/sh
set -e

PKG="{CAMERA_PKG}"
DESIRED="{desired}"
SSC="{super_slow_config}"
CAP_VER="{capability_version}"
FP="$(getprop ro.build.fingerprint)"
if [ -z "$FP" ]; then
    FP="unknown"
fi

DATA_DIR="/data/user/0/$PKG"
if [ ! -d "$DATA_DIR" ]; then
    DATA_DIR="/data/data/$PKG"
fi

PREFS_DIR="$DATA_DIR/shared_prefs"
mkdir -p "$PREFS_DIR"

OWNER_U=$(stat -c %u "$DATA_DIR" 2>/dev/null || echo 0)
OWNER_G=$(stat -c %g "$DATA_DIR" 2>/dev/null || echo 0)

ensure_file() {{
    file="$1"
    if [ ! -f "$file" ]; then
        printf "%s\n" \
            "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>" \
            "<map>" \
            "  <string name=\"android.os.Build.FINGERPRINT\">$FP</string>" \
            "  <int name=\"capability-version\" value=\"$CAP_VER\" />" \
            "  <string name=\"super-slow-values\">$DESIRED</string>" \
                "  <string name=\"sony-super-slow-config\">$SSC</string>" \
            "</map>" \
            > "$file"
    fi

    if grep -q "name=\"android.os.Build.FINGERPRINT\"" "$file"; then
        sed -i "s#<string name=\\\"android.os.Build.FINGERPRINT\\\">[^<]*</string>#<string name=\\\"android.os.Build.FINGERPRINT\\\">$FP</string>#" "$file" || true
    else
        sed -i "s#</map>#  <string name=\\\"android.os.Build.FINGERPRINT\\\">$FP</string>\\n</map>#" "$file" || true
    fi

    if grep -q "name=\"capability-version\"" "$file"; then
        sed -i "s#<int name=\\\"capability-version\\\" value=\\\"[^\\\"]*\\\" */>#<int name=\\\"capability-version\\\" value=\\\"$CAP_VER\\\" />#" "$file" || true
    else
        sed -i "s#</map>#  <int name=\\\"capability-version\\\" value=\\\"$CAP_VER\\\" />\\n</map>#" "$file" || true
    fi

    if grep -q "name=\"super-slow-values\"" "$file"; then
        sed -i "s#<string name=\\\"super-slow-values\\\">[^<]*</string>#<string name=\\\"super-slow-values\\\">$DESIRED</string>#" "$file" || true
    else
        sed -i "s#</map>#  <string name=\\\"super-slow-values\\\">$DESIRED</string>\\n</map>#" "$file" || true
    fi

    if grep -q "name=\"sony-super-slow-config\"" "$file"; then
        sed -i "s#<string name=\\\"sony-super-slow-config\\\">[^<]*</string>#<string name=\\\"sony-super-slow-config\\\">$SSC</string>#" "$file" || true
    else
        sed -i "s#</map>#  <string name=\\\"sony-super-slow-config\\\">$SSC</string>\\n</map>#" "$file" || true
    fi

    chown "$OWNER_U:$OWNER_G" "$file" 2>/dev/null || true
    chmod 660 "$file" 2>/dev/null || true
    restorecon -F "$file" 2>/dev/null || true
}}

# Prefer updating existing supported_values.*.xml files; do not create minimal caches.
FOUND=0
for f in "$PREFS_DIR"/com.sonyericsson.android.camera.supported_values.*.xml; do
    if [ -e "$f" ]; then
        FOUND=1
        ensure_file "$f"
    fi
done

if [ "$FOUND" -eq 0 ]; then
    echo "NO_EXISTING_PREFS"
    exit 2
fi

exit 0
"""

        try:
                with open(host_tmp, "w", encoding="utf-8") as f:
                        f.write(script)
                os.chmod(host_tmp, 0o755)
        except Exception as e:
                sys.stderr.write(f"Failed to write helper script: {e}\n")
                return False

        run(f"{adb} push {host_tmp} {dev_tmp}", check=False)
        p = adb_shell(f"su -c sh {dev_tmp}", check=False)
        if p.returncode != 0:
            return False

        # Best-effort verification: dump the first matching prefs file containing our key.
        verify = adb_shell(
            "su -c '"
            "PKG=com.sonyericsson.android.camera; "
            "D=/data/user/0/$PKG/shared_prefs; [ -d $D ] || D=/data/data/$PKG/shared_prefs; "
            "for f in $D/com.sonyericsson.android.camera.supported_values.*.xml; do "
            "  [ -e $f ] || continue; "
            "  grep -q \"super-slow-values\" $f || continue; "
            "  echo PREF_FILE=$f; "
            "  grep -n \"android.os.Build.FINGERPRINT\\|capability-version\\|super-slow-values\\|sony-super-slow-config\" $f | head -n 8; "
            "  break; "
            "done'",
            check=False,
        )
        if verify.stdout:
            print(verify.stdout.strip())
        if verify.stderr:
            sys.stderr.write(verify.stderr)

        return True


def log_says_super_slow_motion(log: str) -> bool:
    # CameraApp logs mode transitions and states including the literal token
    # "SUPER_SLOW_MOTION". Using logs avoids locale-dependent UI text matching.
    return "SUPER_SLOW_MOTION" in log


def run(cmd: str, check: bool = True, timeout_s: int | None = 30) -> subprocess.CompletedProcess:
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        sys.stderr.write(f"Command timed out: {cmd}\n")
        if check:
            raise SystemExit(124)
        return subprocess.CompletedProcess(args=cmd, returncode=124, stdout="", stderr="timeout")
    if check and p.returncode != 0:
        sys.stderr.write(f"Command failed ({p.returncode}): {cmd}\n")
        if p.stdout:
            sys.stderr.write(p.stdout)
        if p.stderr:
            sys.stderr.write(p.stderr)
        raise SystemExit(p.returncode)
    return p


def adb_shell(cmd: str, check: bool = True) -> subprocess.CompletedProcess:
    return run(f"{adb} shell {cmd}", check=check)


def try_start_intents() -> bool:
    # Preferred: launch directly into (super) slow motion via known extra.
    attempted = False
    for mode in ["SUPER_SLOW_MOTION", "SLOW_MOTION"]:
        for action in ["android.media.action.STILL_IMAGE_CAMERA", "android.media.action.VIDEO_CAMERA"]:
            attempted = True
            run(
                f"{adb} shell am start -W -a {action} -n {CAMERA_ACT} --es {EXTRA_CAMERA_MODE} {mode} --es android.intent.extra.CAMERA_MODE {mode} --ez {EXTRA_CAMERA_OPEN_ONLY} true",
                check=False,
            )
            time.sleep(2)
    # We can't reliably detect success here; return True to indicate we attempted a guided launch.
    if attempted:
        return True

    actions = [
        # baseline
        "android.media.action.VIDEO_CAMERA",
        # best-effort guesses for Sony camera
        f"{CAMERA_PKG}.action.SLOW_MOTION",
        f"{CAMERA_PKG}.action.SLOWMOTION",
        f"{CAMERA_PKG}.action.SUPER_SLOW_MOTION",
        f"{CAMERA_PKG}.action.SUPERSLOW",
    ]

    for action in actions:
        p = run(f"{adb} shell cmd package resolve-activity --brief -a {action}", check=False)
        out = (p.stdout or "") + (p.stderr or "")
        if "No activity found" in out or p.returncode != 0:
            continue

        # If resolve succeeded, try launching it.
        run(f"{adb} shell am start -W -a {action}", check=False)
        return True

    return False


def uiauto_dump(local_path: str) -> bool:
    # uiautomator can intermittently fail while the camera UI is animating or busy.
    for _ in range(3):
        p = adb_shell("uiautomator dump /sdcard/ui.xml", check=False)
        if p.returncode == 0:
            p2 = run(f"{adb} pull /sdcard/ui.xml {local_path}", check=False)
            if p2.returncode == 0 and os.path.exists(local_path):
                return True
        time.sleep(1.0)
    return False


def parse_bounds(bounds: str):
    # format: [l,t][r,b]
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds)
    if not m:
        return None
    left, top, right, bottom = map(int, m.groups())
    return left, top, right, bottom


def center_of(bounds):
    left, top, right, bottom = bounds
    return (left + right) // 2, (top + bottom) // 2


def tap_by_keywords(keywords: list[str]) -> bool:
    tmp = "/tmp/ui.xml"
    if not uiauto_dump(tmp):
        return False

    try:
        root = ET.parse(tmp).getroot()
    except Exception:
        return False

    for node in root.iter():
        text = (node.attrib.get("text") or "").strip().lower()
        desc = (node.attrib.get("content-desc") or "").strip().lower()
        rid = (node.attrib.get("resource-id") or "").strip().lower()
        hay = " ".join([text, desc, rid])

        if any(k.lower() in hay for k in keywords):
            b = node.attrib.get("bounds")
            if not b:
                continue
            pb = parse_bounds(b)
            if not pb:
                continue
            x, y = center_of(pb)
            adb_shell(f"input tap {x} {y}", check=False)
            return True

    return False


def try_tap_slow_motion_via_ui(prefer_super: bool = True) -> bool:
    tmp = "/tmp/ui.xml"
    if not uiauto_dump(tmp):
        return False

    try:
        tree = ET.parse(tmp)
        root = tree.getroot()
    except Exception:
        return False

    # Prefer super slow / 960 if present; otherwise accept slow motion.
    keywords_super = ["super slow", "super_slow", "960", "超慢", "超慢動作", "super slow motion"]
    keywords_slow = ["slow", "slow motion", "慢", "慢動作", "スロー"]
    keywords = keywords_super + keywords_slow if prefer_super else keywords_slow + keywords_super

    best = None
    for node in root.iter():
        text = (node.attrib.get("text") or "").strip().lower()
        desc = (node.attrib.get("content-desc") or "").strip().lower()
        rid = (node.attrib.get("resource-id") or "").strip().lower()

        hay = " ".join([text, desc, rid])
        if any(k in hay for k in keywords):
            b = node.attrib.get("bounds")
            if not b:
                continue
            pb = parse_bounds(b)
            if not pb:
                continue
            best = pb
            break

    if not best:
        return False

    x, y = center_of(best)
    adb_shell(f"input tap {x} {y}", check=False)
    return True


def swipe_modes(max_swipes: int = 6) -> bool:
    # Best-effort: many camera apps switch modes via horizontal swipe.
    # Use a mid-screen swipe to avoid hitting UI chrome.
    for i in range(max_swipes):
        adb_shell("input swipe 900 900 200 900 250", check=False)
        time.sleep(1.0)
        if try_tap_slow_motion_via_ui(prefer_super=True):
            return True
    return False


def try_start_recording_best_effort() -> None:
    # Some modes (esp. super slow) might only configure framerate after record start.
    adb_shell("input keyevent 27", check=False)  # KEYCODE_CAMERA
    time.sleep(1.0)
    adb_shell("input keyevent 27", check=False)


def try_select_960_in_slow_motion_ui() -> bool:
    # Best-effort: try to open the fps/quality chooser then pick 960.
    # Some builds expose 960 directly in the current UI; try that first.
    if tap_by_keywords(["960", "960fps", "960 FPS", "P960", "MOV_SM_960", "SUPER_SLOW_MOTION"]):
        time.sleep(1.0)
        return True

    opened = tap_by_keywords(["120", "120fps", "fps", "hd_120", "HD_120", "120 FPS"])
    if opened:
        time.sleep(1.0)
    picked = tap_by_keywords(["960", "960fps", "960 FPS", "P960", "MOV_SM_960"])
    if picked:
        time.sleep(1.0)
    return picked


def grab_logcat() -> str:
    p = run(f"{adb} logcat -d -v time", check=True)
    return p.stdout or ""


def has_required_logs(log: str) -> bool:
    return bool(MODE_PATTERN.search(log)) and bool(FR_960_PATTERN.search(log))


def wake_and_unlock_best_effort() -> None:
    # If the device is on the lockscreen, uiautomator will only see SystemUI and
    # our keyword taps/swipes won't reach the camera app.
    adb_shell("input keyevent 224", check=False)  # KEYCODE_WAKEUP
    time.sleep(0.5)
    adb_shell("wm dismiss-keyguard", check=False)
    time.sleep(0.5)
    adb_shell("input keyevent 82", check=False)  # KEYCODE_MENU (often unlocks non-secure keyguard)
    time.sleep(0.5)
    adb_shell("input swipe 540 1600 540 400 300", check=False)  # swipe up
    time.sleep(1.0)


def main() -> int:
    run(f"{adb} wait-for-device")

    patch_prefs = "--patch-prefs" in sys.argv
    if "--no-patch-prefs" in sys.argv:
        patch_prefs = False

    wake_and_unlock_best_effort()

    # Clean slate logs
    run(f"{adb} logcat -c", check=False)

    # Restart camera
    adb_shell(f"am force-stop {CAMERA_PKG}", check=False)

    if patch_prefs:
        gate_ok = enable_super_slow_motion_gate_best_effort()
        print(f"super_slow_gate_fix={1 if gate_ok else 0}")
    else:
        print("super_slow_gate_fix=0")

    # First attempt: intent-based (cold start)
    started = try_start_intents()
    print(f"intent_start={1 if started else 0}")
    if started:
        time.sleep(3)
    else:
        # Baseline launch
        run(f"{adb} shell am start -W -n {CAMERA_ACT}", check=False)
        time.sleep(3)

    # If the intent already put us into SUPER_SLOW_MOTION, avoid UI taps that might
    # switch back to STANDARD slow motion via MRU shortcuts.
    log_after_intent = grab_logcat()
    already_super = log_says_super_slow_motion(log_after_intent)
    print(f"detected_super_mode_log={1 if already_super else 0}")

    # Second attempt: UI-based (best-effort)
    tapped = False
    if not already_super:
        tapped = try_tap_slow_motion_via_ui(prefer_super=True)
        if not tapped:
            tapped = try_tap_slow_motion_via_ui(prefer_super=False)
    print(f"ui_tap={1 if tapped else 0}")
    if tapped:
        time.sleep(3)

    # Third attempt: swipe through modes then try UI tap again
    swiped = swipe_modes()
    print(f"swipe_mode_search={1 if swiped else 0}")
    if swiped:
        time.sleep(2)

    selected_960 = try_select_960_in_slow_motion_ui()
    print(f"select_960_ui={1 if selected_960 else 0}")

    # Try to start recording to force framerate negotiation/logging.
    try_start_recording_best_effort()

    # Give nativeGetCaps/getCaps a moment
    time.sleep(2)

    log = grab_logcat()
    interesting = "\n".join(
        line
        for line in log.splitlines()
        if (
            "WRAP:" in line
            or "SLOW_MOTION" in line
            or "SUPER_SLOW_MOTION" in line
            or "framerate:" in line
            or "fps" in line.lower()
        )
    )

    print(interesting)

    if has_required_logs(log):
        print("\nPASS: found required logs: SLOW_MOTION + framerate:960")
        return 0

    print("\nFAIL: missing required logs: SLOW_MOTION and/or framerate:960")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
