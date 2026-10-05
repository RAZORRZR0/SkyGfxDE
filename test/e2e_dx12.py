"""E2E: the overlay (ImGui menu) and post effects work with DE on D3D12 (-dx12, through D3D11On12).

Needs DX12AMDFix.asi in the game folder on GPUs without VRS AdditionalShadingRates (AMD), or DE itself crashes.
Launches DE windowed with -dx12 (the save must load into gameplay by itself), types the rain cheat (drops, grain), shoots
the screen with the look on and off (Toggle hotkey), opens the menu (Menu hotkey) and shoots again, resizes the game
window (ResizeBuffers with the wrapped buffers released), then checks the log and the crash folder. Writes
test/e2e_dx12.txt (pass/fail lines only, so a second run reproduces it); screenshots in %TEMP%.
Usage: py -3.12 test/e2e_dx12.py   (game must be closed; ~2.5 minutes; do not touch mouse/keyboard)"""
import ctypes, os, re, subprocess, sys, tempfile, time
from PIL import ImageChops, ImageGrab, ImageStat

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, chord, focus, pid_of  # noqa: E402
from e2e_postfx import type_text  # noqa: E402

CRASH = os.path.expandvars(r"%USERPROFILE%\Documents\Rockstar Games\GTA San Andreas Definitive Edition\Crashes")
OUT = os.path.join(os.path.dirname(__file__), "e2e_dx12.txt")
TMP = tempfile.gettempdir()
CTRL, SHIFT = 0x11, 0x10


def shot(name):
    im = ImageGrab.grab()
    im.save(os.path.join(TMP, f"e2e_dx12_{name}.png"))
    return im.convert("RGB")


def diff(a, b):  # mean absolute pixel difference, 0..255
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    before = set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()
    ini_path = os.path.join(GAME, "SkyGfxDE.ini")
    ini = open(ini_path, encoding="utf-8").read()
    # the shipped ini logs nothing; TestMode = full SpeedFX all the time, so a blurred HUD would show
    open(ini_path, "w", encoding="utf-8").write(re.sub(r"(?m)^TestMode=.*$", "TestMode=1", re.sub(r"(?m)^Log=.*$", "Log=2", ini, count=1), count=1))
    alive = False
    try:
        subprocess.Popen([os.path.join(GAME, "SanAndreas.exe"), "-dx12", "-windowed", "-ResX=1600", "-ResY=900"], cwd=GAME)
        time.sleep(80)
        pid = pid_of("SanAndreas.exe")
        hwnd = focus(pid); time.sleep(2)
        type_text("AUIFRVQS"); time.sleep(25)          # rain: water drops + grain
        on = shot("look_on")
        chord(CTRL, SHIFT, ord("E")); time.sleep(1.5)  # look off
        off = shot("look_off")
        chord(CTRL, SHIFT, ord("E")); time.sleep(1.5)  # look on
        pre = shot("pre_menu")
        chord(CTRL, SHIFT, ord("M")); time.sleep(1.5)  # menu
        menu = shot("menu")
        chord(CTRL, SHIFT, ord("M")); time.sleep(3)
        chord(0x1B); time.sleep(4)                     # DE pause menu: no 3D scene, so no animated drops/grain on it
        pause1 = shot("pause1"); time.sleep(0.7)
        pause2 = shot("pause2")
        chord(0x1B); time.sleep(4)
        ctypes.windll.user32.SetWindowPos(hwnd, None, 0, 0, 1280, 720, 0x0016); time.sleep(6)  # NOZORDER|NOACTIVATE|NOMOVE
        focus(pid); time.sleep(2)
        shot("after_resize")
        alive = pid_of("SanAndreas.exe") is not None
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
        subprocess.run(["taskkill", "/IM", "CrashReportClient.exe", "/F"], capture_output=True)
        time.sleep(3)
        open(ini_path, "w", encoding="utf-8").write(ini)
    crashes = sorted((set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()) - before)
    log = open(os.path.join(GAME, "SkyGfxDE.log"), encoding="utf-8", errors="replace").read()
    d_look, d_menu, d_pause = diff(on, off), diff(pre, menu), diff(pause1, pause2)
    print(f"look on/off diff {d_look:.2f}, menu diff {d_menu:.2f}, pause menu frame-to-frame diff {d_pause:.2f}")
    checks = [
        ("overlay on D3D12 through D3D11On12", "D3D12 swap chain, drawing through D3D11On12" in log),
        ("post effects drawn under the HUD (scene copied at DE's HUD bind)", "post effects drawn under the HUD" in log),
        ("post effects created", "postfx:" not in log),
        ("look toggle changes the frame (post effects + colours drawn)", d_look > 2.0),
        ("menu hotkey draws the ImGui menu", d_menu > 2.0),
        ("no post effects on DE's pause menu (static between frames)", d_pause < 0.5),
        ("no exceptions", "exception" not in log),
        ("window resize: ResizeBuffers succeeded with the wrapped buffers released",
         re.search(r"ResizeBuffers \d+x\d+ -> 0x00000000", log) is not None and "ResizeBuffers" in log and
         not re.search(r"ResizeBuffers \d+x\d+ -> 0x(?!00000000)", log)),
        ("game alive, no crash report", alive and not crashes),
    ]
    lines = [f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks]
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print("\n".join(l for l in log.splitlines() if re.search("overlay|postfx|speedfx|exception", l)))
    sys.exit(0 if all(ok for _, ok in checks) else 1)


if __name__ == "__main__":
    main()
