"""Frame-time A/B with PresentMon (needs admin for ETW): DX12 without SkyGfxDE, DX12 with it, DX11 with it.
Each run: launch, wait for the save to load, then hold W (CJ runs forward, streaming as in play) for 30 s while PresentMon
records. Writes test/e2e_frametimes.txt: per config frames, average fps, median / p99 frame time and hitches (frames
over 2.5x the median). Usage (elevated): py -3.12 test/e2e_frametimes.py <PresentMon.exe> [configs, e.g. dx12_off,dx12_on]"""
import csv, os, statistics, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(__file__))
from e2e_menu_click import GAME, focus, pid_of, u32  # noqa: E402

PM = sys.argv[1]
WANT = sys.argv[2].split(",") if len(sys.argv) > 2 else ["dx12_off", "dx12_on", "dx11_on"]
OUT = os.path.join(os.path.dirname(__file__), "e2e_frametimes.txt")
ASI = os.path.join(GAME, "SkyGfxDE.asi")
CONFIGS = {"dx12_off": (["-dx12"], False), "dx12_on": (["-dx12"], True), "dx11_on": ([], True)}


def run(name):
    args, sky = CONFIGS[name]
    if not sky: os.rename(ASI, ASI + ".abaoff")
    csv_path = os.path.join(tempfile.gettempdir(), f"pm_{name}.csv")
    if os.path.exists(csv_path): os.remove(csv_path)
    try:
        subprocess.Popen([os.path.join(GAME, "SanAndreas.exe")] + args, cwd=GAME)
        time.sleep(85)
        focus(pid_of("SanAndreas.exe")); time.sleep(2)
        pm = subprocess.Popen([PM, "--process_name", "SanAndreas.exe", "--output_file", csv_path, "--v1_metrics", "--timed", "30",
                               "--terminate_after_timed", "--no_console_stats", "--stop_existing_session"])
        time.sleep(1)
        walk = not os.environ.get("IDLE")  # IDLE=1: no input at all (the AFK / mouse-only case)
        if walk: u32.keybd_event(0x57, 0, 0, 0)  # hold W
        pm.wait(timeout=60)
        if walk: u32.keybd_event(0x57, 0, 2, 0)
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
        while pid_of("SanAndreas.exe"): time.sleep(1)
        if not sky: os.rename(ASI + ".abaoff", ASI)
    ms = [float(r["msBetweenPresents"]) for r in csv.DictReader(open(csv_path, encoding="utf-8-sig"))]
    med = statistics.median(ms)
    p99 = sorted(ms)[int(len(ms) * 0.99)]
    hitches = sum(m > 2.5 * med for m in ms)
    return f"{name}: frames {len(ms)}, avg {1000 * len(ms) / sum(ms):.1f} fps, median {med:.2f} ms, p99 {p99:.2f} ms, " \
           f"max {max(ms):.1f} ms, hitches (>2.5x median) {hitches}"


if pid_of("SanAndreas.exe"): sys.exit("close the game first")
lines = []
for n in WANT:
    lines.append(run(n)); print(lines[-1], flush=True)
open(OUT, "w").write("\n".join(lines) + "\n")
