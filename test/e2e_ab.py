"""E2E A/B: shadow darkness and matte characters change the picture in the real game.

Launches DE (the save must load into gameplay by itself), then for each setting writes it to the installed ini,
reloads it with Ctrl+Shift+R and takes a screenshot. A setting passes when its A/B difference is clearly larger than
the difference between two shots with the same settings (animation and cloud noise). Writes test/e2e_ab.txt (pass/fail
lines only, repeatable); shots go to %TEMP%. The ini values are restored at the end.
Usage: py -3.12 test/e2e_ab.py   (game must be closed; ~2 minutes; do not touch mouse/keyboard)"""
import ctypes, os, subprocess, sys, tempfile, time
from PIL import ImageChops, ImageGrab, ImageStat

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, chord, focus, k32 as K, module_base, pid_of, read  # noqa: E402
import struct

INI = os.path.join(GAME, "SkyGfxDE.ini")
OUT = os.path.join(os.path.dirname(__file__), "e2e_ab.txt")
k32 = ctypes.windll.kernel32
buf = ctypes.create_unicode_buffer(64)


def get(sec, key):
    k32.GetPrivateProfileStringW(sec, key, "", buf, 64, INI)
    return buf.value


def shot(name, settings=None):
    for (sec, key), v in (settings or {}).items():
        k32.WritePrivateProfileStringW(sec, key, str(v), INI)
    chord(0x11, 0x10, 0x52)  # Ctrl+Shift+R: reload ini
    time.sleep(3)
    img = ImageGrab.grab()
    img.save(os.path.join(tempfile.gettempdir(), f"e2e_ab_{name}.png"))
    return img


def diff(a, b, box):  # mean absolute difference, 0..255, in a crop
    return sum(ImageStat.Stat(ImageChops.difference(a.crop(box), b.crop(box))).mean) / 3


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    saved = {k: get(*k) for k in (("Shadows", "Darkness"), ("Characters", "Matte"))}
    subprocess.Popen([os.path.join(GAME, "SanAndreas.exe")], cwd=GAME)
    time.sleep(80)
    pid = pid_of("SanAndreas.exe")
    lines = []
    try:
        focus(pid); time.sleep(2)
        chord(0x46); time.sleep(4)  # F: get out of the car the save starts in, so CJ stands in the middle
        hp = K.OpenProcess(0x0410, False, pid); mb = module_base(pid)
        def q(a): r = read(hp, a, 8); return struct.unpack("<Q", r)[0] if r else 0
        def fl(a): r = read(hp, a, 4); return struct.unpack("<f", r)[0] if r else float("nan")
        def sky():  # TOD live SkylightColor alpha, TOD SkyLightIntensity, sky light component intensity
            tod = q(q(mb + 0x5724750) + 0x688); comp = q(q(tod + 0x5F0) + 544)
            return f"alpha {fl(tod + 0x2C4):.3f} todIntensity {fl(tod + 0x66C):.3f} component {fl(comp + 524):.3f}"
        D, M = ("Shadows", "Darkness"), ("Characters", "Matte")
        a = shot("base1", {D: 0, M: 0})
        b = shot("base2"); sky_b = sky()
        w, h = a.size
        full, cj = (0, h // 6, w, h), (w * 2 // 5, h // 3, w * 3 // 5, h * 9 // 10)
        noise_full, noise_cj = diff(a, b, full), diff(a, b, cj)
        dark = shot("dark", {D: 0.9}); sky_d = sky()
        print("sky light at darkness 0:", sky_b, "| at 0.9:", sky_d)
        lum = lambda img: ImageStat.Stat(img.crop(full).convert("L")).mean[0]
        matte = shot("matte", {D: 0, M: 1})
        d_dark, d_matte = diff(b, dark, full), diff(b, matte, cj)
        lines += [
            f"{'PASS' if d_dark > 3 * noise_full + 1 and lum(dark) < lum(b) else 'FAIL'} shadow darkness 0 -> 0.9 darkens the frame",
            f"{'PASS' if d_matte > 3 * noise_cj + 1 else 'FAIL'} matte 0 -> 1 changes the characters",
        ]
        print(f"noise full {noise_full:.2f} cj {noise_cj:.2f} | darkness diff {d_dark:.2f}, luma {lum(b):.1f} -> {lum(dark):.1f} | matte diff {d_matte:.2f}")
    except Exception as e:
        lines.append(f"FAIL {e}")
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
        for (sec, key), v in saved.items():
            k32.WritePrivateProfileStringW(sec, key, v or None, INI)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    sys.exit(0 if all(l.startswith("PASS") for l in lines) else 1)


if __name__ == "__main__":
    main()
