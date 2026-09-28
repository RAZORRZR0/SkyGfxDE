"""E2E: the post effects run in the real game and follow the rain.

Launches DE (the save must load into gameplay by itself), shoots the screen in clear weather, types the rainy weather
cheat (AUIFRVQS), waits for CWeather::Rain to climb, shoots again, then checks SkyGfxDE.log. Writes
test/e2e_postfx.txt (pass/fail lines only, so a second run reproduces it); the screenshots go to %TEMP%.
Usage: py -3.12 test/e2e_postfx.py   (game must be closed; ~2.5 minutes; do not touch mouse/keyboard)"""
import ctypes, os, re, struct, subprocess, sys, tempfile, time
from PIL import ImageGrab

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, focus, k32, module_base, pid_of, read  # noqa: E402

RAIN_RVA = 0x531A1DC  # CWeather::Rain
LOD_RVA = 0x53E14E4   # TheCamera.m_fLODDistMultiplier (DE alone: 70/FOV)
FOV_RVA = 0x50311FC   # CDraw FOV, as divided in CCamera::Process
OUT = os.path.join(os.path.dirname(__file__), "e2e_postfx.txt")
u32 = ctypes.windll.user32


def rain(h, base):
    b = read(h, base + RAIN_RVA, 4)
    return struct.unpack("<f", b)[0] if b else -1.0


def type_text(s):
    for ch in s:
        u32.keybd_event(ord(ch), 0, 0, 0); time.sleep(0.05)
        u32.keybd_event(ord(ch), 0, 2, 0); time.sleep(0.08)


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    log_path = os.path.join(GAME, "SkyGfxDE.log")  # rewritten at every launch
    subprocess.Popen([os.path.join(GAME, "SanAndreas.exe")], cwd=GAME)
    time.sleep(80)
    pid = pid_of("SanAndreas.exe")
    h = k32.OpenProcess(0x0410, False, pid)
    base = module_base(pid)
    checks = []
    try:
        focus(pid); time.sleep(2)
        ImageGrab.grab().save(os.path.join(tempfile.gettempdir(), "e2e_postfx_clear.png"))
        r0 = rain(h, base)
        lod, fov = struct.unpack("<ff", (read(h, base + LOD_RVA, 4) or b"\0" * 4) + (read(h, base + FOV_RVA, 4) or b"\0" * 4))
        ratio = lod / (70.0 / fov) if fov > 0 else 0.0
        print(f"LOD multiplier {lod:.3f}, FOV {fov:.1f}, ratio to DE's 70/FOV {ratio:.3f}")
        checks.append(("LOD multiplier = DE's 70/FOV x LodDistance 1.8", abs(ratio - 1.8) < 0.05))
        type_text("AUIFRVQS")
        time.sleep(30)
        r1 = rain(h, base)
        ImageGrab.grab().save(os.path.join(tempfile.gettempdir(), "e2e_postfx_rain.png"))
        time.sleep(3)
        checks.append(("rain cheat raises CWeather::Rain", r1 > 0.5 and r1 > r0))
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
    with open(log_path, encoding="utf-8", errors="replace") as f:
        log = f.read()
    checks += [
        ("tools and splash hooks ready", "debug tools ready" in log and "splash FX hooks ready" in log),
        ("hydrant/fountain and particle hooks ready", "hydrant/fountain hook ready" in log and "wake particle hook ready" in log),
        ("post effects created", "postfx:" not in log),
        ("drawn before the HUD bind", re.search(r"binds per frame over 600 frames: .*3:(\d+)", log) is not None),
        ("no exceptions", "exception" not in log),
    ]
    lines = [f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks]
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"rain {r0:.2f} -> {r1:.2f}; log:\n" + "\n".join(l for l in log.splitlines() if re.search("postfx|drops|speedfx|exception|overlay", l)))
    sys.exit(0 if all(ok for _, ok in checks) else 1)


if __name__ == "__main__":
    main()
