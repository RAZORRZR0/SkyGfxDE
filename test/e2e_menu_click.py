"""E2E: clicking inside the SkyGfxDE menu must not turn the game camera.
Launches GTA SA DE, opens the menu (Ctrl+Shift+M), clicks inside the ImGui window, and compares TheCamera's
forward vector (read from process memory) before and after. Writes test/e2e_menu_click.txt; exit 0 = pass.
Usage: py -3.12 test/e2e_menu_click.py   (game must be closed; takes ~2 minutes; do not touch mouse/keyboard)"""
import ctypes, ctypes.wintypes as W, math, os, re, struct, subprocess, sys, time

GAME = r"D:\GTA SA\GTA San Andreas - Definitive Edition\Gameface\Binaries\Win64"
CAM_MATRIX_RVA = 0x53E13F8  # TheCamera.m_matrix (CMatrix*): right, forward, up, pos rows of 16 bytes
OUT = os.path.join(os.path.dirname(__file__), "e2e_menu_click.txt")
u32, k32 = ctypes.windll.user32, ctypes.windll.kernel32
k32.OpenProcess.restype = W.HANDLE


def pid_of(name):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    m = re.search(r'"%s","(\d+)"' % re.escape(name), out)
    return int(m.group(1)) if m else 0


def module_base(pid):
    class ME(ctypes.Structure):
        _fields_ = [("dwSize", W.DWORD), ("th32ModuleID", W.DWORD), ("th32ProcessID", W.DWORD), ("GlblcntUsage", W.DWORD),
                    ("ProccntUsage", W.DWORD), ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", W.DWORD), ("hModule", W.HMODULE),
                    ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]
    k32.CreateToolhelp32Snapshot.restype = W.HANDLE
    snap = k32.CreateToolhelp32Snapshot(0x18, pid)  # TH32CS_SNAPMODULE | SNAPMODULE32
    me = ME(); me.dwSize = ctypes.sizeof(ME)
    ok = k32.Module32First(W.HANDLE(snap), ctypes.byref(me))
    k32.CloseHandle(W.HANDLE(snap))
    return me.modBaseAddr if ok else 0


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(W.HANDLE(h), ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw if ok else None


def forward(h, base):
    p = read(h, base + CAM_MATRIX_RVA, 8)
    m = p and read(h, struct.unpack("<Q", p)[0], 64)
    return struct.unpack_from("<3f", m, 16) if m else None


def yaw(f): return math.degrees(math.atan2(f[1], f[0]))


def focus(pid):
    hw = []
    cb = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)(lambda w, l: (hw.append(w) if u32.IsWindowVisible(w) and _pid(w) == pid else None) or True)
    u32.EnumWindows(cb, 0)
    if hw:
        u32.keybd_event(0x12, 0, 0, 0); u32.keybd_event(0x12, 0, 2, 0)  # Alt tap lets SetForegroundWindow succeed
        u32.SetForegroundWindow(hw[0])
    return hw[0] if hw else None


def _pid(w):
    p = W.DWORD(); u32.GetWindowThreadProcessId(w, ctypes.byref(p)); return p.value


def chord(*vks):
    for v in vks: u32.keybd_event(v, 0, 0, 0); time.sleep(0.03)
    time.sleep(0.15)
    for v in reversed(vks): u32.keybd_event(v, 0, 2, 0); time.sleep(0.03)


def click(x, y):
    u32.SetCursorPos(x, y); time.sleep(0.1)
    if "--moves-only" in sys.argv: time.sleep(0.4); return
    u32.mouse_event(0x2, 0, 0, 0, 0); time.sleep(0.08); u32.mouse_event(0x4, 0, 0, 0, 0); time.sleep(0.3)


def menu_rect():
    txt = open(os.path.join(GAME, "SkyGfxDE_imgui.ini"), encoding="utf-8", errors="replace").read()
    m = re.search(r"\[Window\]\[SkyGfxDE\]\s*Pos=(-?\d+),(-?\d+)\s*Size=(\d+),(\d+)", txt)
    return tuple(map(int, m.groups())) if m else (60, 60, 840, 1020)


def main():
    if pid_of("SanAndreas.exe"): sys.exit("close the game first")
    subprocess.Popen([os.path.join(GAME, "SanAndreas.exe")], cwd=GAME)
    time.sleep(70)
    pid = pid_of("SanAndreas.exe")
    h = k32.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    base = module_base(pid)
    lines = []
    try:
        hw = focus(pid); time.sleep(1)
        f0 = forward(h, base)
        if not f0: raise RuntimeError("camera matrix unreadable (not in gameplay yet?)")
        chord(0x11, 0x10, 0x4D); time.sleep(1)  # Ctrl+Shift+M
        rect = ctypes.wintypes.RECT(); u32.GetClientRect(hw, ctypes.byref(rect))
        pt = W.POINT(0, 0); u32.ClientToScreen(hw, ctypes.byref(pt))
        x, y, w, hgt = menu_rect()
        yaws = [yaw(f0)]
        for i in range(6):  # clicks spread over the window body (tabs / empty space / slider track)
            click(pt.x + x + 60 + 90 * i, pt.y + y + 120 + 40 * i)  # no re-focus here: UE re-grabs the mouse on focus
        time.sleep(1)
        f1 = forward(h, base); yaws.append(yaw(f1))
        chord(0x11, 0x10, 0x4D); time.sleep(1.5)  # close menu, game resumes with the real cursor
        f2 = forward(h, base); yaws.append(yaw(f2))
        d_menu = abs((yaws[1] - yaws[0] + 180) % 360 - 180)
        d_after = abs((yaws[2] - yaws[0] + 180) % 360 - 180)
        ok = d_menu < 2.0 and d_after < 2.0
        lines += [f"yaw before clicks: {yaws[0]:.1f}", f"yaw after 6 clicks in menu: {yaws[1]:.1f} (delta {d_menu:.1f})",
                  f"yaw after closing menu: {yaws[2]:.1f} (delta {d_after:.1f})", "PASS" if ok else "FAIL: camera turned"]
    except Exception as e:
        ok = False; lines.append(f"FAIL: {e}")
    finally:
        subprocess.run(["taskkill", "/IM", "SanAndreas.exe", "/F"], capture_output=True)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
