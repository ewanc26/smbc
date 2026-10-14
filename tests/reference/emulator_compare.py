#!/usr/bin/env python3
"""Frame-by-frame comparison of the port against a reference NES emulator (#31).

Dev-only tool. It needs nes-py 8.2.1 with NumPy 1.26.x (NumPy 2 breaks
nes-py's PRG size arithmetic). The ROM is never copied into the repo; set
SMBC_ROM to the path of your own copy of the cartridge.

    python3 tests/reference/emulator_compare.py [scenario ...]

Each scenario runs from power-on for the same number of frames as the
recording in compare.c. Frame i of the emulator is compared with frame i
of the port, both counted from power-on, after mapping each port palette
index to the emulator colour it corresponds to. The mapping is the
majority colour for each index over the run; the exact-frame check is
what decides pass or fail.

Header fix: the cartridge in the reference set has garbage in header
bytes 12-15, which nes-py rejects. The emulator reads a copy with those
four bytes zeroed. PRG and CHR are copied unchanged, and the copy lives
in a temporary directory.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FRAME_DUMP_SRC = os.path.join(REPO, "tools", "frame_dump.c")
FRAME_DUMP_BIN = os.path.join(REPO, "build", "tools", "frame_dump")

# (frames, input script) — the same scripts as tests/reference/compare.c
def _idle(frame):
    return 0x00

def _start(frame):
    return 0x08 if frame == 200 else 0x00  # Start at frame 200

def _walk(frame):
    if frame == 200:
        return 0x08
    if frame >= 700:
        return 0x80  # hold Right
    return 0x00

def _jump(frame):
    if frame == 200:
        return 0x08
    if frame >= 700:
        return 0x81 if (frame % 90) < 12 else 0x80  # A taps over Right
    return 0x00

SCENARIOS = {
    "title": (90, _idle),
    "start": (450, _start),
    "demo": (700, _idle),
    "walk": (950, _walk),
    "jump": (950, _jump),
}

W, H = 256, 240


def check_emulator():
    try:
        import nes_py  # noqa: F401
        from importlib.metadata import version
    except ImportError as exc:
        sys.exit(f"emulator_compare: nes-py is not installed ({exc}); see the module docstring")
    nes_version = version("nes-py")
    numpy_version = version("numpy")
    if nes_version != "8.2.1" or not numpy_version.startswith("1."):
        sys.exit(f"emulator_compare: need nes-py 8.2.1 and numpy 1.x, found {nes_version} and {numpy_version}")


def fixed_rom_copy(rom_path, workdir):
    data = bytearray(open(rom_path, "rb").read())
    if data[:4] != b"NES\x1a":
        sys.exit("emulator_compare: SMBC_ROM is not an iNES file")
    data[12:16] = b"\x00\x00\x00\x00"
    out = os.path.join(workdir, "smb_header_fixed.nes")
    open(out, "wb").write(bytes(data))
    return out


def build_port_dumper():
    os.makedirs(os.path.dirname(FRAME_DUMP_BIN), exist_ok=True)
    srcs = subprocess.run(
        ["find", os.path.join(REPO, "src"), "-name", "*.c", "!", "-path", "*/src/host/*"],
        capture_output=True, text=True, check=True).stdout.split()
    cmd = ["cc", "-std=c23", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O2",
           "-o", FRAME_DUMP_BIN, FRAME_DUMP_SRC] + srcs
    subprocess.run(cmd, check=True)


def emulator_frames(rom, inputs):
    from nes_py import NESEnv
    env = NESEnv(rom)
    env.reset()
    frames = np.zeros((len(inputs), H, W, 3), dtype=np.uint8)
    for i, b in enumerate(inputs):
        env.step(b)
        frames[i] = env.screen
    env.close()
    return frames


def port_frames(input_path, count, out_path):
    subprocess.run([FRAME_DUMP_BIN, input_path, out_path, str(count)], check=True)
    return np.fromfile(out_path, dtype=np.uint8).reshape(count, H, W)


def palette_map(emu, port):
    """Map each port index to the emulator colour it most often appears as.
    A pixel that disagrees with the majority is a mismatch, so the frame
    comparison below is what decides pass or fail."""
    from collections import Counter, defaultdict
    pairs = defaultdict(Counter)
    for i in range(len(port)):
        e = emu[i].reshape(-1, 3)
        p = port[i].reshape(-1)
        keys = (e[:, 0].astype(np.int64) << 16) | (e[:, 1].astype(np.int64) << 8) | e[:, 2]
        for idx in np.unique(p):
            for k, c in Counter(keys[p == idx].tolist()).items():
                pairs[int(idx)][k] += c
    lut = np.zeros((64, 3), dtype=np.uint8)
    for idx, counts in pairs.items():
        k = counts.most_common(1)[0][0]
        lut[idx] = [(k >> 16) & 255, (k >> 8) & 255, k & 255]
    return lut


def run_scenario(name, rom, workdir):
    frames, script = SCENARIOS[name]
    inputs = [script(f) for f in range(frames)]
    input_path = os.path.join(workdir, f"{name}.bin")
    with open(input_path, "wb") as fh:
        fh.write(bytes(inputs))
    emu = emulator_frames(rom, inputs)
    port = port_frames(input_path, frames, os.path.join(workdir, f"{name}_port.bin"))
    lut = palette_map(emu, port)
    mismatched = []
    for i in range(frames):
        if not (lut[port[i]] == emu[i]).all():
            mismatched.append(i)
    return frames, mismatched


def main(argv):
    rom = os.environ.get("SMBC_ROM")
    if not rom or not os.path.isfile(rom):
        sys.exit("emulator_compare: set SMBC_ROM to the path of the cartridge (.nes)")
    check_emulator()
    names = argv or list(SCENARIOS)
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        sys.exit(f"emulator_compare: unknown scenario(s) {unknown}; choose from {list(SCENARIOS)}")
    build_port_dumper()
    failed = False
    with tempfile.TemporaryDirectory(prefix="smbc_emu_") as workdir:
        fixed = fixed_rom_copy(rom, workdir)
        for name in names:
            frames, mismatched = run_scenario(name, fixed, workdir)
            exact = frames - len(mismatched)
            print(f"{name}: {exact}/{frames} frames exact", end="")
            if mismatched:
                failed = True
                print(f"; first mismatches {mismatched[:8]}", end="")
            print()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
