r"""E2E: [ModernLook] options (SSGI/GTAO/SSR/volumetric fog, sun strength), each independent.

Two launches (-dx12) at Grove Street, EXTRASUNNY_LA, via test/e2e_modern.js (13:00, then 17:30): "off" with every
option off and "on" with every option on and SunStrength=1.5 (SkyGfxDE.ini restored afterwards). Writes
test/e2e_modern.txt (pass/fail + mean luminance per shot + the cvars found); shots in %TEMP%\e2e_modern_<cfg>_<time>.png.
Usage: py -3.12 test/e2e_modern.py   (game closed; ~5 minutes; hands off)"""
import os, re, shutil, subprocess, sys, tempfile, time
from PIL import ImageGrab, ImageStat

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, focus, pid_of  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), "e2e_modern.txt")
TMP = tempfile.gettempdir()
JS = os.path.join(GAME, "CLEO", "zz_modern.js")
INI = os.path.join(GAME, "SkyGfxDE.ini")
CRASH = os.path.expandvars(r"%USERPROFILE%\Documents\Rockstar Games\GTA San Andreas Definitive Edition\Crashes")


def set_key(text, sec, key, val):
    if re.search(rf"(?m)^{key}=", text): return re.sub(rf"(?m)^{key}=.*$", f"{key}={val}", text, count=1)
    return text.replace(f"[{sec}]", f"[{sec}]\n{key}={val}", 1) if f"[{sec}]" in text else text + f"\n[{sec}]\n{key}={val}\n"


def shot(name):
    im = ImageGrab.grab(); im.save(os.path.join(TMP, f"e2e_modern_{name}.png"))
    return round(ImageStat.Stat(im.convert("L")).mean[0], 1)


def run(name, modern):
    orig = open(INI, encoding="utf-8").read()
    before = set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()
    ini = set_key(orig, "General", "Log", 1)
    for k, v in (("SSGI", 1), ("AmbientOcclusion", 1), ("Reflections", 1), ("VolumetricFog", 1)):
        ini = set_key(ini, "ModernLook", k, int(modern) * v)
    ini = set_key(ini, "ModernLook", "SunStrength", 1.5 if modern else 1.0)
    open(INI, "w", encoding="utf-8").write(ini)
    res = {"alive": False}
    try:
        subprocess.Popen([os.path.join(GAME, "SanAndreas.exe"), "-dx12"], cwd=GAME)
        time.sleep(75)
        if pid_of("SanAndreas.exe"):
            focus(pid_of("SanAndreas.exe")); time.sleep(30)   # 13:00 held from script +20 s to +90 s
            res["1300"] = shot(f"{name}_1300")
            time.sleep(70)                                      # 17:30 held after +90 s
            res["1730"] = shot(f"{name}_1730")
            res["alive"] = bool(pid_of("SanAndreas.exe"))
    finally:
        subprocess.run("taskkill /IM SanAndreas.exe /F", shell=True, capture_output=True)
        time.sleep(3)
        open(INI, "w", encoding="utf-8").write(orig)
    crashed = bool((set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()) - before)
    log = open(os.path.join(GAME, "SkyGfxDE.log"), encoding="utf-8", errors="replace").read()
    lines = [f"{'PASS' if res['alive'] and not crashed else 'FAIL'} {name}: alive={res['alive']} crash={crashed} "
             f"luminance 17:30={res.get('1730')} 13:00={res.get('1300')}"]
    if modern:
        found = re.findall(r"modern: (r\.\S+) (found|NOT found)", log)
        lines.append(f"{'PASS' if found and all(s == 'found' for _, s in found) else 'FAIL'} cvars: " + ", ".join(f"{n} {s}" for n, s in found))
        lines += [l.split('] ', 1)[-1] for l in log.splitlines() if "modern: r." in l][:6]
    return lines


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    shutil.copy(os.path.join(os.path.dirname(__file__), "e2e_modern.js"), JS)
    try:
        lines = run("off", False) + run("on", True)
    finally:
        os.remove(JS)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
