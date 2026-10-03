"""E2E: the in-engine look (DE's GTA fog path + timecyc sky) is stable and works for every weather.

Launches DE (the save must load into gameplay by itself), freezes the clock, then:
- 22:00 SUNNY_SF, standing still: 12 shots 0.1 s apart differ little (no flicker);
- every one of the 23 weathers at 00:00 and 12:00: night sky dark (no blown-up colour), midday sky not dark, and the
  log has a brightness calibration line for each weather and no exceptions.
Writes test/e2e_weathers.txt (pass/fail lines only, repeatable); shots go to %TEMP%.
Usage: py -3.12 test/e2e_weathers.py   (game must be closed; ~4 minutes; do not touch mouse/keyboard)"""
import ctypes, os, re, struct, subprocess, sys, tempfile, time
from PIL import ImageChops, ImageGrab, ImageStat

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, chord, focus, k32 as K, module_base, pid_of  # noqa: E402

LOG = os.path.join(GAME, "SkyGfxDE.log")
OUT = os.path.join(os.path.dirname(__file__), "e2e_weathers.txt")
TMP = tempfile.gettempdir()
K.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
HOURS, MINUTES, MS_PER_MIN, WEATHERS = 0x521270B, 0x521270F, 0x522AD00, (0x5300000, 0x52FFFF0, 0x5300018)
SUNNY_SF = 5
luma = lambda c: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def diff(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    subprocess.Popen([os.path.join(GAME, "SanAndreas.exe")], cwd=GAME)
    time.sleep(80)
    pid = pid_of("SanAndreas.exe")
    lines = []
    try:
        focus(pid); time.sleep(2)
        chord(0x46); time.sleep(4)  # F: out of the car the save starts in
        hp = K.OpenProcess(0x0438, False, pid); mb = module_base(pid)
        def w(a, fmt, v): b = struct.pack(fmt, v); K.WriteProcessMemory(hp, mb + a, b, len(b), None)
        def weather(n):
            for a in WEATHERS: w(a, "<h", n)
        def hour(h): w(HOURS, "<B", h); w(MINUTES, "<B", 0)
        w(MS_PER_MIN, "<I", 1000000)  # freeze the clock
        weather(SUNNY_SF); hour(22); time.sleep(6)
        shots = []
        for _ in range(12):  # the HUD corner is cropped out (the clock digits change)
            img = ImageGrab.grab(); wd, ht = img.size
            shots.append(img.crop((0, 0, wd * 3 // 4, ht)))
            time.sleep(0.1)
        shots[0].save(os.path.join(TMP, "e2e_weathers_still.png"))
        noise = max(diff(shots[i], shots[i + 1]) for i in range(11))
        print(f"still frame noise {noise:.2f}")
        lines.append(f"{'PASS' if noise < 2 else 'FAIL'} frame stable standing still (22:00 SUNNY_SF)")
        dark_bad, bright_bad = [], []
        for n in range(23):
            weather(n)
            for h in (0, 12):
                hour(h); time.sleep(3)
                img = ImageGrab.grab(); wd, ht = img.size
                img.save(os.path.join(TMP, f"e2e_weathers_w{n:02d}_{h:02d}.png"))
                sky = ImageStat.Stat(img.crop((wd * 5 // 100, ht * 3 // 100, wd * 70 // 100, ht * 25 // 100))).mean
                print(f"weather {n:2d} {h:02d}:00 sky {[round(x) for x in sky]}")
                if h == 0 and n not in (19, 21) and luma(sky) > 90: dark_bad.append(n)  # sandstorm / underwater excluded
                if h == 12 and luma(sky) < 40: bright_bad.append(n)
        log = open(LOG, encoding="latin1").read()
        calibrated = {int(m) for m in re.findall(r"brightness: weather (\d+) ", log)}
        lines += [
            f"{'PASS' if not dark_bad else 'FAIL'} 00:00 sky dark in every weather {dark_bad or ''}".rstrip(),
            f"{'PASS' if not bright_bad else 'FAIL'} 12:00 sky not dark in any weather {bright_bad or ''}".rstrip(),
            f"{'PASS' if calibrated >= set(range(23)) else 'FAIL'} brightness calibrated for all 23 weathers",
            f"{'PASS' if 'exception' not in log else 'FAIL'} no exceptions in the log",
        ]
    except Exception as e:
        lines.append(f"FAIL {e}")
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    sys.exit(0 if all(l.startswith("PASS") for l in lines) else 1)


if __name__ == "__main__":
    main()
