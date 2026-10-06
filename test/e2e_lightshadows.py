"""E2E: [StreetLights] OtherLightShadows on every world light, SF Chinatown lanterns lit up close.

Needs CLEO Redux and CLEO/zz_lamptest.js (sets 23:00 and teleports to SF Chinatown 20 s after load). Launches DE
(-dx12), shoots with the look on and off (Ctrl+Shift+E), then checks the log and the crash folder. Writes
test/e2e_lightshadows.txt (pass/fail lines only); screenshots in %TEMP%.
Usage: py -3.12 test/e2e_lightshadows.py   (game closed; ~2 minutes; hands off)"""
import os, re, subprocess, sys, tempfile, time
from PIL import ImageGrab, ImageStat

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, chord, focus, pid_of  # noqa: E402

CRASH = os.path.expandvars(r"%USERPROFILE%\Documents\Rockstar Games\GTA San Andreas Definitive Edition\Crashes")
OUT = os.path.join(os.path.dirname(__file__), "e2e_lightshadows.txt")
TMP = tempfile.gettempdir()


def shot(name):
    im = ImageGrab.grab(); im.save(os.path.join(TMP, f"e2e_lights_{name}.png"))
    return ImageStat.Stat(im.convert("L")).mean[0]


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    before = set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()
    ini_path = os.path.join(GAME, "SkyGfxDE.ini")
    ini = open(ini_path, encoding="utf-8").read()
    open(ini_path, "w", encoding="utf-8").write(re.sub(r"(?m)^Log=.*$", "Log=1", ini, count=1))
    res = {}
    try:
        subprocess.Popen([os.path.join(GAME, "SanAndreas.exe"), "-dx12"], cwd=GAME)
        time.sleep(75)
        focus(pid_of("SanAndreas.exe")); time.sleep(30)  # teleport at +20 s, streaming, lamps on
        res["on"] = shot("on")
        chord(0x11, 0x10, ord("E")); time.sleep(4)      # look off: DE's lamps (no shadows)
        res["off"] = shot("off")
        chord(0x11, 0x10, ord("E")); time.sleep(4)
        res["on2"] = shot("on2")
        res["alive"] = bool(pid_of("SanAndreas.exe"))
    finally:
        subprocess.run("taskkill /IM SanAndreas.exe /F", shell=True, capture_output=True)
        time.sleep(3)
        open(ini_path, "w", encoding="utf-8").write(ini)
    log = open(os.path.join(GAME, "SkyGfxDE.log"), encoding="utf-8", errors="replace").read()
    m = [tuple(map(int, x)) for x in re.findall(r"light shadows: (\d+) lights, (\d+) fixture meshes", log)]
    crashes = (set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()) - before
    lines = [
        f"{'PASS' if res.get('alive') else 'FAIL'} game alive through the toggles",
        f"{'PASS' if not crashes else 'FAIL'} no new crash report",
        f"{'PASS' if 'exception' not in log.lower() else 'FAIL'} no exception in SkyGfxDE.log",
        f"{'PASS' if m and max(a for a, _ in m) > 0 else 'FAIL'} lights given shadows",
        f"{'PASS' if m and max(b for _, b in m) > 0 else 'FAIL'} fixture meshes without shadow",
    ]
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines)); print("brightness on/off/on2:", res.get("on"), res.get("off"), res.get("on2"), "log:", m[-3:])


if __name__ == "__main__":
    main()
