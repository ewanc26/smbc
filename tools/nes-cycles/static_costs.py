"""Static base cycles per smb.asm instruction, checked against the ROM run (#41).

    python3 static_costs.py ../../smb.asm PROFILE_PREFIX...

Base cycles come from BASE in cpu6502.c, so there is one copy of the table.
PROFILE_PREFIX is an nes_trace run with NES_PROFILE=1 (its -pc.csv is read).
An executed instruction's measured cycles per execution must be at least its
base cost, and exactly equal unless a branch or an indexed read can add a
penalty (taken branch, page crossing). Any other excess is reported."""
import csv
import os
import re
import sys

import asm_labels

HERE = os.path.dirname(os.path.abspath(__file__))
PENALTY_MODES = {'indexed': ('absx', 'absy', 'indy')}


def base_table():
    text = open(os.path.join(HERE, 'cpu6502.c'), encoding='utf-8').read()
    body = text.split('BASE[256] = {', 1)[1].split('};', 1)[0]
    values = [int(v) for v in re.findall(r'(?<![0-9A-Fa-f])(\d+)(?=\s*,)', body)]
    if len(values) != 256:
        raise SystemExit('BASE table parse: %d entries' % len(values))
    return values


def may_penalise(mn, mode):
    if mode == 'rel':
        return True
    if mode in PENALTY_MODES['indexed'] and mn not in ('STA', 'STX', 'STY', 'DEC', 'INC', 'ASL', 'LSR', 'ROL', 'ROR'):
        return True
    return False


def main(argv):
    asm_path = argv[1]
    prefixes = argv[2:]
    asm, _ = asm_labels.build(asm_path)
    base = base_table()
    static = {a: (mn, mode, op, base[op]) for a, (mn, mode, op) in asm.insns.items()}
    print('instructions:', len(static))

    measured = {}
    for prefix in prefixes:
        for r in csv.DictReader(open(prefix + '-pc.csv')):
            a = int(r['address'], 16)
            c, n = int(r['cycles']), int(r['executions'])
            cyc, ex = measured.get(a, (0, 0))
            measured[a] = (cyc + c, ex + n)

    below, exact, penalised, unexplained = [], 0, 0, []
    for a, (cyc, ex) in sorted(measured.items()):
        if a not in static:
            unexplained.append((a, 'no instruction at this address'))
            continue
        mn, mode, op, b = static[a]
        per = cyc / ex
        if per < b:
            below.append((a, mn, mode, b, per))
        elif per == b:
            exact += 1
        elif may_penalise(mn, mode):
            penalised += 1
        else:
            unexplained.append((a, '%s %s base %d measured %.2f' % (mn, mode, b, per)))

    print('executed addresses:', len(measured))
    print('  exactly base:', exact)
    print('  base + branch/page penalty:', penalised)
    print('  below base (table wrong):', len(below))
    for row in below[:20]:
        print('    %04x %s %s base=%d measured=%.2f' % (row[0], row[1], row[2], row[3], row[4]))
    print('  unexplained excess or missing:', len(unexplained))
    for row in unexplained[:20]:
        print('    %04x %s' % (row[0], row[1]))
    return 1 if below or unexplained else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
