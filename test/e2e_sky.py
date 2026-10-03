"""E2E: the night sky follows the timecyc, not DE's time-of-day overrides (DE keeps an orange sunset until ~23:00).

Launches DE (the save must load into gameplay by itself), forces EXTRASUNNY_VEGAS and freezes the clock, then at 22:00
and 12:00 takes screenshots with [Colours] Brightness 0 and 1 (Ctrl+Shift+R reloads the ini) and reads
AGTATimeOfDay's live SkyLower colour. Needs [Timecyc] File = the PS2 timecyc (22:00 sky bottom 18,15,44).
Writes test/e2e_sky.txt (pass/fail lines only, repeatable); shots go to %TEMP%. The ini value is restored at the end.
Usage: py -3.12 test/e2e_sky.py   (game must be closed; ~2.5 minutes; do not touch mouse/keyboard)"""
import ctypes, os, struct, subprocess, sys, tempfile, time
from PIL import ImageGrab, ImageStat

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, chord, focus, k32 as K, module_base, pid_of, read  # noqa: E402

INI = os.path.join(GAME, "SkyGfxDE.ini")
OUT = os.path.join(os.path.dirname(__file__), "e2e_sky.txt")
k32 = ctypes.windll.kernel32
K.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
EXTRASUNNY_VEGAS, SUNNY_SF = 11, 5
# gta_sa DE RVAs (README): CWeather Old/New/Forced 0x5300000/0x52FFFF0/0x5300018 (written below), CClock hours/minutes,
# ms per game minute, engine singleton
HOURS, MINUTES, MS_PER_MIN, SINGLETON = 0x521270B, 0x521270F, 0x522AD00, 0x5724750


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    buf = ctypes.create_unicode_buffer(64)
    k32.GetPrivateProfileStringW("Colours", "Brightness", "", buf, 64, INI)
    saved = buf.value
    subprocess.Popen([os.path.join(GAME, "SanAndreas.exe")], cwd=GAME)
    time.sleep(80)
    pid = pid_of("SanAndreas.exe")
    lines = []
    try:
        focus(pid); time.sleep(2)
        chord(0x46); time.sleep(4)  # F: out of the car the save starts in
        hp = K.OpenProcess(0x0438, False, pid); mb = module_base(pid)
        def w(a, fmt, v): b = struct.pack(fmt, v); K.WriteProcessMemory(hp, mb + a, b, len(b), None)
        def q(a): r = read(hp, a, 8); return struct.unpack("<Q", r)[0] if r else 0
        def live(off):  # live FSkyColorSet field (AGTATimeOfDay +0x2B8): rgb x alpha
            tod = q(q(mb + SINGLETON) + 0x688)
            r, g, b, a = struct.unpack("<4f", read(hp, tod + 0x2B8 + off, 16))
            return r * a, g * a, b * a
        w(MS_PER_MIN, "<I", 1000000)  # freeze the clock
        for a in (0x5300000, 0x52FFFF0, 0x5300018): w(a, "<h", EXTRASUNNY_VEGAS)
        res = {}
        for hour in (22, 12):
            w(HOURS, "<B", hour); w(MINUTES, "<B", 0)
            for b in (0, 1):
                k32.WritePrivateProfileStringW("Colours", "Brightness", str(b), INI)
                chord(0x11, 0x10, 0x52)  # Ctrl+Shift+R: reload ini
                time.sleep(6)  # DE's eye adaptation settles
                img = ImageGrab.grab(); img.save(os.path.join(tempfile.gettempdir(), f"e2e_sky_{hour:02d}_b{b}.png"))
                wd, ht = img.size
                # the save starts at the Four Dragons facing the Strip: open sky between the casino and the HUD
                sky = ImageStat.Stat(img.crop((wd * 45 // 100, ht * 3 // 100, wd * 75 // 100, ht * 30 // 100))).mean
                ground = ImageStat.Stat(img.crop((wd * 25 // 100, ht * 75 // 100, wd * 75 // 100, ht * 95 // 100))).mean
                res[hour, b] = (sky, live(0x10), live(0x158), ground)
                print(hour, b, "sky", [round(x) for x in sky], "ground", [round(x) for x in ground],
                      "SkyLower", [round(x, 4) for x in live(0x10)], "Sun", [round(x, 3) for x in live(0x158)])
        # 00:00 SUNNY_SF: timecyc sky top (0, 8, 12) while DE's SkyUpper alpha is 0 at night; Brightness once divided by
        # that alpha and turned the whole night sky bright cyan.
        for a in (0x5300000, 0x52FFFF0, 0x5300018): w(a, "<h", SUNNY_SF)
        w(HOURS, "<B", 0); w(MINUTES, "<B", 0)
        time.sleep(6)
        img = ImageGrab.grab(); img.save(os.path.join(tempfile.gettempdir(), "e2e_sky_00_sf.png"))
        wd, ht = img.size
        sf = ImageStat.Stat(img.crop((wd * 45 // 100, ht * 3 // 100, wd * 75 // 100, ht * 30 // 100))).mean
        tod = q(q(mb + SINGLETON) + 0x688)
        upper = struct.unpack("<4f", read(hp, tod + 0x2B8 + 0x20, 16))
        print("00 SUNNY_SF sky", [round(x) for x in sf], "SkyUpper", [round(x, 4) for x in upper])
        luma = lambda c: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
        (n0, _, _, g0), (n1, live1, sun1, g1) = res[22, 0], res[22, 1]
        (d0, _, sund0, _), (d1, _, sund1, _) = res[12, 0], res[12, 1]
        lines += [
            f"{'PASS' if luma(n1) < 0.6 * luma(n0) else 'FAIL'} 22:00 sky with Brightness 1 is much darker than with 0",
            f"{'PASS' if luma(g1) < 0.8 * luma(g0) else 'FAIL'} 22:00 street with Brightness 1 is darker (NightExposure)",
            f"{'PASS' if n1[2] > n1[1] else 'FAIL'} 22:00 sky with Brightness 1 is purple (blue > green), not DE's orange sunset",
            f"{'PASS' if live1[2] > live1[0] and live1[2] < 0.05 else 'FAIL'} 22:00 live SkyLower is the timecyc's dark purple",
            f"{'PASS' if abs(luma(d1) - luma(d0)) < 0.25 * luma(d0) else 'FAIL'} 12:00 sky brightness stays within 25% of DE's",
            f"{'PASS' if max(sun1) == 0 else 'FAIL'} 22:00 sun is off with Brightness 1 (original night from 21:00)",
            f"{'PASS' if max(sund1) > 0.5 * max(sund0) else 'FAIL'} 12:00 sun stays on with Brightness 1",
            f"{'PASS' if luma(sf) < 60 and max(upper[:3]) < 1.0 else 'FAIL'} 00:00 SUNNY_SF sky is dark (not bright cyan)",
        ]
    except Exception as e:
        lines.append(f"FAIL {e}")
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
        k32.WritePrivateProfileStringW("Colours", "Brightness", saved or None, INI)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    sys.exit(0 if all(l.startswith("PASS") for l in lines) else 1)


if __name__ == "__main__":
    main()
