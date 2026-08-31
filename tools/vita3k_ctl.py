#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vita3k_ctl — los ojos y los brazos de MAGI sobre Vita3K.

Recreación del controlador perdido con la sesión del 30-ago-2026, esta vez
versionado en el repo (lección F-5 del megaplan R1). Todo lo que el ciclo de
rondas necesita hacer SIN manos:

  lanzar    Vita3K sin elevar y sin el OpenSSL de Git en el PATH (A10)
  arrancar  la app YabauseVita (YABA00001) por línea de comandos
  pulsar    teclas en la ventana de Vita3K (Circle=C, Start=Enter, ...)
  leer      yabausevita_log.txt por ventanas de 5 s → FPS / GPU / EMU
  editar    config.cfg (rom_path, autostart, ...) SIN BOM — con BOM el
            emulador ignora rom_path silenciosamente (hallazgo de la R1)
  matar     el proceso al terminar la corrida

Acotación deliberada: las pulsaciones solo se envían a ventanas cuyo título
empieza por "Vita3K". Un inyector de teclas sin destino acotado no tiene
modo de fallo razonable.

Uso:
  python tools/vita3k_ctl.py config --rom "ux0:data/yabause/roms/X.chd" --autostart 1
  python tools/vita3k_ctl.py run --seconds 40            # lanza, mide, mata, imprime JSON
  python tools/vita3k_ctl.py launch / kill / metrics / key C

Sin dependencias: solo ctypes y la librería estándar.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# ── Rutas (parametrizables por CLI, con defaults del repo) ────────────
REPO = Path(__file__).resolve().parent.parent
VITA3K_EXE = REPO / "vita3k" / "Vita3K.exe"
PREF = Path(os.environ.get("APPDATA", "")) / "Vita3K" / "Vita3K"
APP_DIR = PREF / "ux0" / "app" / "YABA00001"
LOG = PREF / "ux0" / "data" / "yabausevita_log.txt"
CONFIG = PREF / "ux0" / "data" / "yabause" / "config.cfg"

BOOT_MARKER = "YabauseVita starting"

# ── Scancodes set-1 de las teclas que usa el mapeo por defecto ────────
SCAN = {
    "Q": 0x10, "W": 0x11, "E": 0x12, "R": 0x13, "T": 0x14, "Y": 0x15,
    "U": 0x16, "I": 0x17, "O": 0x18, "P": 0x19,
    "A": 0x1E, "S": 0x1F, "D": 0x20, "F": 0x21, "G": 0x22, "H": 0x23,
    "J": 0x24, "K": 0x25, "L": 0x26,
    "Z": 0x2C, "X": 0x2D, "C": 0x2E, "V": 0x2F,
    "ENTER": 0x1C, "RSHIFT": 0x36,
    "UP": 0x48, "LEFT": 0x4B, "RIGHT": 0x4D, "DOWN": 0x50,
}

# ── Win32 ─────────────────────────────────────────────────────────────
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class INPUT(ctypes.Structure):
    class _I(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT)]
    _anonymous_ = ("i",)
    _fields_ = [("type", wt.DWORD), ("i", _I)]


WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def windows_of_pid(pid):
    """Todas las ventanas visibles de un proceso: [(hwnd, titulo, ancho, alto)]."""
    found = []

    @WNDENUMPROC
    def cb(hwnd, _lparam):
        wpid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            rect = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(rect))
            found.append((hwnd, buf.value, rect.right, rect.bottom))
        return True

    user32.EnumWindows(cb, 0)
    return found


def find_game_window(pid=None):
    """La ventana DONDE corre el juego, no el GUI. Vita3K abre una ventana
    de render aparte con el cliente al tamano nativo Vita (960x544)."""
    candidates = windows_of_pid(pid) if pid else []
    if not candidates:  # sin pid: fallback por titulo (sin juego en marcha)
        hwnd = find_vita3k_window()
        return hwnd
    for hwnd, _title, w, h in candidates:
        if abs(w - 960) <= 4 and abs(h - 544) <= 4:
            return hwnd
    # sin ventana 960x544: la que NO sea el GUI principal (con menu File)
    gui = {hwnd for hwnd, t, w, h in candidates if w > 1000 and h > 600}
    non_gui = [hwnd for hwnd, t, w, h in candidates if hwnd not in gui]
    return (non_gui or [c[0] for c in candidates])[0]


def find_vita3k_window():
    """HWND de la primera ventana visible cuyo título empieza por Vita3K."""
    found = []

    @WNDENUMPROC
    def cb(hwnd, _lparam):
        length = user32.GetWindowTextLengthW(hwnd)
        if length > 0:
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            if buf.value.startswith("Vita3K") and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
                return False
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def send_scancode(hwnd, code, hold=0.05):
    """Un par down/up a la ventana hwnd (que debe estar en primer plano)."""
    for flag in (0, KEYEVENTF_KEYUP):
        inp = INPUT(type=INPUT_KEYBOARD)
        inp.ki = KEYBDINPUT(0, code, KEYEVENTF_SCANCODE | flag, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(hold)


def press_key(name, hold=0.05):
    """Enfoca la ventana DEL JUEGO (AttachThreadInput) y pulsa una tecla."""
    if name.upper() not in SCAN:
        raise SystemExit(f"tecla no mapeada: {name}")
    hwnd = find_game_window(CURRENT_PID) or find_vita3k_window()
    if not hwnd:
        raise SystemExit("no hay ventana Vita3K* abierta")
    cur_thread = kernel32.GetCurrentThreadId()
    pid = wt.DWORD()
    target_thread = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    user32.AttachThreadInput(cur_thread, target_thread, True)
    try:
        user32.SetForegroundWindow(hwnd)
        user32.SetFocus(hwnd)
        time.sleep(0.15)
        send_scancode(hwnd, SCAN[name.upper()], hold)
    finally:
        user32.AttachThreadInput(cur_thread, target_thread, False)


# ── Ojos: captura de la ventana y detección de movimiento ────────────
# El FPS no es prueba de que haya imagen: la Ronda 1 lo pago — 60 FPS
# emulados con la pantalla en negro. Una corrida valida exige capturas
# continuas y movimiento real entre ellas.
from PIL import Image, ImageChops

PW_RENDERFULLCONTENT = 0x00000002  # captura contenido GPU (OpenGL/D3D)


def capture_window(hwnd=None):
    """PIL Image del área cliente de la ventana Vita3K (PrintWindow)."""
    hwnd = hwnd or find_vita3k_window()
    if not hwnd:
        raise SystemExit("no hay ventana Vita3K* abierta")
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    w, h = rect.right, rect.bottom
    hdc_window = user32.GetDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_window)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_window, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)
    result = user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)

    class BITMAP(ctypes.Structure):
        _fields_ = [("bmType", wt.LONG), ("bmWidth", wt.LONG),
                    ("bmHeight", wt.LONG), ("bmWidthBytes", wt.LONG),
                    ("bmPlanes", wt.WORD), ("bmBitsPixel", wt.WORD),
                    ("bmBits", wt.LPVOID)]
    bmp = BITMAP()
    gdi32.GetObjectW(hbmp, ctypes.sizeof(bmp), ctypes.byref(bmp))
    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]
    class BMI(ctypes.Structure):
        _fields_ = [("bmiHeader", BMIH), ("bmiColors", wt.DWORD * 3)]
    bmi = BMI()
    bmi.bmiHeader.biSize = ctypes.sizeof(BMIH)
    bmi.bmiHeader.biWidth = bmp.bmWidth
    bmi.bmiHeader.biHeight = -bmp.bmHeight  # top-down
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = 0  # BI_RGB
    buf = ctypes.create_string_buffer(bmp.bmWidth * bmp.bmHeight * 4)
    gdi32.GetDIBits(hdc_mem, hbmp, 0, bmp.bmHeight, buf, ctypes.byref(bmi), 0)
    img = Image.frombytes("RGBA", (bmp.bmWidth, bmp.bmHeight), buf.raw)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_window)
    return img.convert("RGB"), bool(result)


def frame_stats(img, prev):
    """% de píxeles casi negros y % que cambió respecto a la captura previa."""
    gray = img.convert("L")
    hist = gray.histogram()
    black = sum(hist[:16]) / (img.width * img.height) * 100.0
    diff = None
    if prev is not None:
        d = ImageChops.difference(gray, prev.convert("L"))
        changed = sum(d.histogram()[13:]) / (img.width * img.height) * 100.0
        diff = changed
    return black, diff


def monitor(seconds=40, interval=2.5, out_dir=None, tag="", con_oidos=True):
    """Capturas continuas de la ventana DEL JUEGO → veredicto de imagen,
    movimiento y (si hay sounddevice) sonido. Los PNG quedan en out_dir
    para verificación humana."""
    from datetime import datetime
    stamp = datetime.now().strftime("%H%M%S")
    out_dir = Path(out_dir) if out_dir else REPO / "build-docker" / f"shots-{tag or stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    hwnd = find_game_window(CURRENT_PID)
    if not hwnd:
        raise SystemExit("no se encontro ventana de juego (pid=%s)" % CURRENT_PID)
    orejas = None
    if con_oidos and _OIDOS_DISPONIBLES:
        try:
            orejas = Oidos()
            orejas.empezar()
            orejas._stream.start_stream()
        except Exception:
            orejas = None
    shots = []
    prev = None
    t_end = time.time() + seconds
    i = 0
    while time.time() < t_end:
        img, pw_ok = capture_window(hwnd)
        black, diff = frame_stats(img, prev)
        name = f"{out_dir / f'{i:03d}_b{black:05.1f}_d{diff if diff is not None else -1:05.1f}.png'}"
        img.save(name)
        shots.append({"t": round(i * interval, 1), "black_pct": round(black, 2),
                      "diff_pct": round(diff, 2) if diff is not None else None,
                      "printwindow_ok": pw_ok, "file": str(name)})
        print(f"[{i*interval:5.1f}s] negro={black:6.2f}%  cambio={diff if diff is not None else '  ---'}%")
        prev = img
        i += 1
        time.sleep(interval)
    blacks = [s["black_pct"] for s in shots]
    diffs = [s["diff_pct"] for s in shots if s["diff_pct"] is not None]
    verdict = {
        "shots": len(shots),
        "has_image": (sum(1 for b in blacks if b < 90.0) / len(blacks)) >= 0.5 if blacks else False,
        "has_motion": (sum(1 for d in diffs if d > 0.5) / len(diffs)) >= 0.3 if diffs else False,
        "black_pct_mediana": sorted(blacks)[len(blacks)//2] if blacks else None,
        "diff_pct_mediana": sorted(diffs)[len(diffs)//2] if diffs else None,
        "diffs_por_captura": diffs,
        "dir": str(out_dir),
    }
    if orejas is not None:
        verdict["oidos"] = orejas.parar()
    return verdict


# ── Oídos: loopback WASAPI + veredicto de sonido ─────────────────────
# R9 era «sin imagen no hay corrida»; el sonido es la otra mitad. Un juego
# con el audio caído o entrecortado también está roto, y el log no lo
# muestra: scsp_th puede quemar CPU produciendo audio que nunca llega.
try:
    import pyaudiowpatch as _pyaudio
    import numpy as _np
    _OIDOS_DISPONIBLES = True
    _OIDOS_FALTA = ""
except Exception as _e:  # opcional, como pygame/capstone en MAGI
    _OIDOS_DISPONIBLES = False
    _OIDOS_FALTA = str(_e)


class Oidos:
    """Escucha el loopback WASAPI de la salida por defecto mientras el
    juego corre. pyaudiowpatch (no sounddevice): es el que expone el
    loopback de verdad en Windows.

    uso:
        o = Oidos(); o.empezar(); ...correr...; v = o.parar()
    veredicto: has_sound (energia sostenida), choppy (tramos con sonido
    seguidos de silencio, varias veces), rms y sonando_pct.
    """

    def __init__(self, tramo_ms=100):
        self.tramo_ms = tramo_ms
        self._chunks = []
        self._pa = None
        self._stream = None

    def empezar(self):
        if not _OIDOS_DISPONIBLES:
            raise SystemExit(f"oidos sin pyaudiowpatch/numpy: {_OIDOS_FALTA}")
        self._pa = _pyaudio.PyAudio()
        salida = self._pa.get_default_output_device_info()
        prefijo = salida["name"][:20]
        lb = None
        for d in self._pa.get_loopback_device_info_generator():
            if d["name"].startswith(prefijo) or prefijo in d["name"]:
                lb = d
                break
        if lb is None:  # cualquier loopback disponible
            lb = next(self._pa.get_loopback_device_info_generator())
        self.sr = int(lb["defaultSampleRate"])
        self.canal = lb["maxInputChannels"]
        tramo = self.sr * self.tramo_ms // 1000
        self._stream = self._pa.open(
            format=_pyaudio.paFloat32, channels=self.canal, rate=self.sr,
            input=True, input_device_index=lb["index"],
            frames_per_buffer=tramo, stream_callback=self._cb)

    def _cb(self, in_data, frame_count, time_info, status):
        x = _np.frombuffer(in_data, dtype=_np.float32)
        if self.canal >= 2:
            x = x.reshape(-1, self.canal).mean(axis=1)
        self._chunks.append(x.copy())
        return (in_data, _pyaudio.paContinue)

    def parar(self):
        if self._stream:
            self._stream.stop_stream()
            self._stream.close()
            self._stream = None
        if self._pa:
            self._pa.terminate()
            self._pa = None
        if not self._chunks:
            return {"has_sound": False, "choppy": None, "error": "sin captura"}
        x = _np.concatenate(self._chunks)
        tramo = self.sr * self.tramo_ms // 1000
        n = len(x) // tramo
        rms = [float(_np.sqrt(_np.mean(x[i*tramo:(i+1)*tramo]**2)))
               for i in range(n)]
        sonando = [r > 0.004 for r in rms]  # ~-48 dBFS por tramo de 100 ms
        frac = sum(sonando) / len(sonando) if sonando else 0
        cortes = sum(1 for a, b in zip(sonando, sonando[1:]) if a and not b)
        return {
            "tramos": len(rms),
            "has_sound": frac >= 0.3,
            "choppy": bool(frac >= 0.3 and cortes >= 8),
            "rms_mediana": round(sorted(rms)[len(rms)//2], 5),
            "sonando_pct": round(frac * 100, 1),
            "cortes": cortes,
        }


def oidos(segundos=15):
    """Oír N segundos AHORA (la ventana debe estar sonando)."""
    o = Oidos()
    o.empezar()
    o._stream.start_stream()
    time.sleep(segundos)
    return o.parar()


# ── Lanzamiento ───────────────────────────────────────────────────────
def clean_env():
    """PATH sin Git/mingw64: Vita3K resolvería su OpenSSL contra el de Git
    y moriría con EVP_MD_CTX_get_size_ex (A10)."""
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(
        p for p in env.get("PATH", "").split(os.pathsep)
        if "mingw64" not in p.lower() and ("\\git\\" not in p.lower())
    )
    return env


CURRENT_PID = None


def launch(app_dir=None):
    global CURRENT_PID
    kill()
    # -r YABA00001: flag dedicado de Vita3K para arrancar una app instalada
    # (mas limpio que el content-path posicional, que reinstalaria).
    args = [str(VITA3K_EXE), "-r", APP_DIR.name]
    proc = subprocess.Popen(args, env=clean_env(),
                            cwd=str(VITA3K_EXE.parent),
                            creationflags=subprocess.DETACHED_PROCESS)
    CURRENT_PID = proc.pid
    return wait_log(BOOT_MARKER, timeout=90)


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "Vita3K.exe"],
                   capture_output=True)
    time.sleep(1.0)


# ── Log ───────────────────────────────────────────────────────────────
def log_size():
    return LOG.stat().st_size if LOG.exists() else 0


def read_new_text(offset):
    if not LOG.exists():
        return ""
    with LOG.open("r", encoding="utf-8", errors="replace") as f:
        f.seek(offset)
        return f.read()


def wait_log(pattern, offset=None, timeout=60, poll=0.5):
    """Espera a que `pattern` aparezca en el log tras `offset`."""
    offset = log_size() if offset is None else offset
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pattern in read_new_text(offset):
            return True
        time.sleep(poll)
    return False


RE_FPS = re.compile(r"FPS:\s*([\d.]+)")
RE_GPU_NEW = re.compile(
    r"GPU:\s*drawn=(\d+)\s+presented=(\d+)\s+dropped=(\d+)\s+"
    r"composite=(\d+)us\s+upload=(\d+)us\s+display=(\d+)us")
RE_GPU_OLD = re.compile(
    r"GPU timing:\s*composite=(\d+)us\s+upload=(\d+)us\s+display=(\d+)us\s+frames=(\d+)")
RE_EMU = re.compile(r"(\w+)=(\d+)us")
EMU_LINE = "EMU:"


def parse_windows(text):
    """Trocea el log en ventanas de 5 s: dict por ventana con FPS/GPU/EMU."""
    windows = []
    cur = None
    for line in text.splitlines():
        line = line.strip()
        m = RE_FPS.search(line)
        if m:
            cur = {"fps": float(m.group(1))}
            windows.append(cur)
            continue
        m = RE_GPU_NEW.search(line)
        if m and cur is not None:
            cur.update(drawn=int(m.group(1)), presented=int(m.group(2)),
                       dropped=int(m.group(3)), composite_us=int(m.group(4)),
                       upload_us=int(m.group(5)), display_us=int(m.group(6)))
            continue
        m = RE_GPU_OLD.search(line)
        if m and cur is not None:
            cur.update(composite_us=int(m.group(1)),
                       upload_us=int(m.group(2)),
                       display_us=int(m.group(3)),
                       frames=int(m.group(4)))
            continue
        if EMU_LINE in line and cur is not None:
            cur["emu"] = {k: int(v) for k, v in RE_EMU.findall(line)}
            continue
    return windows


def summarize(windows):
    """Mediana de FPS y medias de los acumuladores por ventana."""
    if not windows:
        return {}
    fps = sorted(w["fps"] for w in windows)
    med = fps[len(fps) // 2]
    out = {"windows": len(windows), "fps_median": med,
           "fps_min": fps[0], "fps_max": fps[-1]}
    for key in ("drawn", "presented", "dropped", "composite_us",
                "upload_us", "display_us", "frames"):
        vals = [w[key] for w in windows if key in w]
        if vals:
            out[key + "_avg"] = sum(vals) / len(vals)
    emu_keys = set()
    for w in windows:
        emu_keys.update(w.get("emu", {}).keys())
    for k in emu_keys:
        vals = [w["emu"][k] for w in windows if k in w.get("emu", {})]
        if vals:
            out["emu_" + k + "_avg_us"] = sum(vals) / len(vals)
    return out


ERROR_PAT = re.compile(r"FATAL|Yabause ERROR|\[-2\]|Unsupported CD image")


def scan_errors(text):
    return [l.strip() for l in text.splitlines() if ERROR_PAT.search(l)]


# ── config.cfg ────────────────────────────────────────────────────────
def set_config(updates):
    """Reescribe config.cfg aplicando `updates`. SIN BOM: con BOM, el
    sscanf del emulador lee la primera clave como '\\ufeffrom_path' y la
    ignora — rom_path y autostart morirían en silencio."""
    lines = []
    if CONFIG.exists():
        raw = CONFIG.read_bytes()
        if raw.startswith(b"\xef\xbb\xbf"):
            raw = raw[3:]  # quitar BOM si lo hubiera
        lines = raw.decode("utf-8", errors="replace").splitlines()
    done = set()
    out = []
    for line in lines:
        key = line.split("=", 1)[0].strip() if "=" in line else None
        if key in updates:
            out.append(f"{key}={updates[key]}")
            done.add(key)
        else:
            out.append(line)
    for key, val in updates.items():
        if key not in done:
            out.append(f"{key}={val}")
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_bytes(("\n".join(out) + "\n").encode("utf-8"))


# ── Comandos ──────────────────────────────────────────────────────────
def cmd_run(args):
    if args.rom:
        set_config({"rom_path": args.rom, "autostart": 1})
    elif args.autostart is not None:
        set_config({"autostart": args.autostart})

    offset = log_size()
    if not launch(APP_DIR):
        print(json.dumps({"ok": False,
                          "error": "la app no arrancó (sin marker en log)"}))
        sys.exit(1)

    deadline = time.time() + args.seconds
    while time.time() < deadline:
        time.sleep(1.0)
        if len(parse_windows(read_new_text(offset))) >= args.windows:
            break

    time.sleep(1.0)
    text = read_new_text(offset)
    kill()
    windows = parse_windows(text)
    result = {
        "ok": len(windows) >= min(args.windows, 1),
        "boot": True,
        "errors": scan_errors(text),
        "summary": summarize(windows),
        "windows": windows,
    }
    print(json.dumps(result, ensure_ascii=False, indent=1))
    sys.exit(0 if result["ok"] and not result["errors"] else 2)


def cmd_config(args):
    updates = {}
    if args.rom:
        updates["rom_path"] = args.rom
    if args.autostart is not None:
        updates["autostart"] = args.autostart
    if not updates:
        raise SystemExit("nada que hacer: usa --rom y/o --autostart")
    set_config(updates)
    print(f"config.cfg actualizado: {updates}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("run", help="corrida completa → JSON")
    p.add_argument("--rom", help="rom_path a fijar antes de lanzar")
    p.add_argument("--autostart", type=int, default=None)
    p.add_argument("--seconds", type=int, default=40)
    p.add_argument("--windows", type=int, default=6, help="ventanas de 5 s mínimas")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("config", help="editar config.cfg sin BOM")
    p.add_argument("--rom")
    p.add_argument("--autostart", type=int, default=None)
    p.set_defaults(fn=cmd_config)

    p = sub.add_parser("launch", help="arrancar Vita3K + YabauseVita")
    p.set_defaults(fn=lambda a: print("boot:", launch(APP_DIR)))

    p = sub.add_parser("kill", help="matar Vita3K")
    p.set_defaults(fn=lambda a: kill())

    p = sub.add_parser("metrics", help="volcar ventanas del log actual")
    p.set_defaults(fn=lambda a: print(json.dumps(
        summarize(parse_windows(LOG.read_text(encoding='utf-8', errors='replace'))),
        indent=1)))

    p = sub.add_parser("key", help="pulsar una tecla (C, ENTER, ...)")
    p.add_argument("name")
    p.add_argument("--hold", type=float, default=0.05)
    p.set_defaults(fn=lambda a: press_key(a.name, a.hold))

    p = sub.add_parser("shot", help="una captura de la ventana Vita3K")
    p.set_defaults(fn=lambda a: capture_window()[0].save("vita3k_shot.png") or print("vita3k_shot.png"))

    p = sub.add_parser("oidos", help="escuchar N segundos y veredicto de sonido")
    p.add_argument("--segundos", type=float, default=10)
    p.set_defaults(fn=lambda a: print(json.dumps(oidos(a.segundos),
                                                  ensure_ascii=False, indent=1)))

    p = sub.add_parser("monitor", help="capturas continuas + veredicto de movimiento")
    p.add_argument("--seconds", type=float, default=40)
    p.add_argument("--interval", type=float, default=2.5)
    p.add_argument("--tag", default="")
    p.set_defaults(fn=lambda a: print(json.dumps(
        monitor(a.seconds, a.interval, tag=a.tag), ensure_ascii=False, indent=1)))

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
