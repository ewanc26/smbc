# Reference comparison baselines (#36)

Each scenario is a replay recording (`<name>.smbcr`, see #37) plus a
baseline frame (`<name>.ppm`, P6, 256×240) captured at a fixed frame
number. `tests/reference/compare.c` — run by `make test` — replays
the recording from a fresh reset with the per-frame state hash
checked, renders the checkpoint frame and compares it against the
baseline pixel by pixel.

## Baseline provenance

These baselines are **self-captured from this port**, not from NES
hardware or a reference emulator. They pin current behaviour so
fidelity regressions are measurable; they do not prove correctness
against the original game. When emulator- or hardware-verified
captures exist, replace the PPMs in place and record the source here.

| Scenario | Frames | Checkpoint | Script |
|---|---|---|---|
| `title` | 90 | 89 | idle — title screen |
| `demo` | 700 | 699 | idle — attract demo running |
| `start` | 450 | 449 | Start at 200 — World 1-1 start |
| `walk` | 950 | 949 | Start at 200, hold Right from 700 |
| `jump` | 950 | 949 | Start at 200, Right from 700 with A taps |

## Updating baselines

```sh
./build/tests/reference/compare --update
```

This regenerates recordings and baselines from the compiled-in input
scripts. It is intentionally loud and manual: never re-baseline to
hide a regression. Review the diff outputs
(`build/reference/<name>-{actual,diff}.ppm`) and the git diff of the
baselines before committing, and explain baseline changes in the
commit message.

## Emulator comparison (dev-only, #31)

`tests/reference/emulator_compare.py` compares the port with a reference
NES emulator, frame by frame, from power-on. It is a development tool and
is not part of `make test`.

Setup (dev-only; nothing here is vendored into the repo):

```sh
python3 -m venv .venv-emu
.venv-emu/bin/pip install nes-py==8.2.1 "numpy<2"   # numpy 1.26.x
SMBC_ROM=/path/to/smb.nes .venv-emu/bin/python tests/reference/emulator_compare.py
```

- `nes-py` 8.2.1 requires NumPy 1.x. Under NumPy 2 its PRG size arithmetic
  overflows (`OverflowError: Python integer 1024 out of bounds for uint8`).
- The ROM is never copied into the repo. `SMBC_ROM` points at your own
  cartridge image.
- **Header fix:** the reference cartridge has non-zero bytes at header
  offsets 12-15, which nes-py rejects ("ROM header zero fill bytes are not
  zero"). The script zeroes those four bytes in a temporary copy. PRG and
  CHR are copied unchanged. Your file is not modified.
- The port side is `tools/frame_dump.c`, a dev tool outside `src/`, `tests/`
  and the Makefile. It writes one palette-index frame per emulated frame,
  counted from power-on, with the reset frames blank.
- Palette indices are mapped to emulator colours by majority vote over the
  run. The exact-frame check decides pass or fail.

Status: the harness runs all five scenarios. The port is not yet identical
to the emulator. Frames 0-5 match; the title sequence then runs about three
frames ahead. See #31 for the open timing work.
