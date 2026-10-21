"""Assemble smb.asm's subset and map its labels to ROM addresses (#41).

    python3 asm_labels.py ../../smb.asm ../../smb.nes [--syms]

Two passes; the assembled PRG bytes are compared with the ROM, so a wrong
label address or instruction size is reported as a mismatch."""
import re
import sys

OPS = {
    'ADC': dict(imm=0x69, zp=0x65, zpx=0x75, abs=0x6d, absx=0x7d, absy=0x79, indx=0x61, indy=0x71),
    'AND': dict(imm=0x29, zp=0x25, zpx=0x35, abs=0x2d, absx=0x3d, absy=0x39, indx=0x21, indy=0x31),
    'ASL': dict(acc=0x0a, zp=0x06, zpx=0x16, abs=0x0e, absx=0x1e),
    'BCC': dict(rel=0x90), 'BCS': dict(rel=0xb0), 'BEQ': dict(rel=0xf0), 'BMI': dict(rel=0x30),
    'BNE': dict(rel=0xd0), 'BPL': dict(rel=0x10), 'BVC': dict(rel=0x50), 'BVS': dict(rel=0x70),
    'BIT': dict(zp=0x24, abs=0x2c),
    'BRK': dict(imp=0x00),
    'CLC': dict(imp=0x18), 'CLD': dict(imp=0xd8), 'CLI': dict(imp=0x58), 'CLV': dict(imp=0xb8),
    'CMP': dict(imm=0xc9, zp=0xc5, zpx=0xd5, abs=0xcd, absx=0xdd, absy=0xd9, indx=0xc1, indy=0xd1),
    'CPX': dict(imm=0xe0, zp=0xe4, abs=0xec), 'CPY': dict(imm=0xc0, zp=0xc4, abs=0xcc),
    'DEC': dict(zp=0xc6, zpx=0xd6, abs=0xce, absx=0xde),
    'DEX': dict(imp=0xca), 'DEY': dict(imp=0x88),
    'EOR': dict(imm=0x49, zp=0x45, zpx=0x55, abs=0x4d, absx=0x5d, absy=0x59, indx=0x41, indy=0x51),
    'INC': dict(zp=0xe6, zpx=0xf6, abs=0xee, absx=0xfe),
    'INX': dict(imp=0xe8), 'INY': dict(imp=0xc8),
    'JMP': dict(abs=0x4c, ind=0x6c), 'JSR': dict(abs=0x20),
    'LDA': dict(imm=0xa9, zp=0xa5, zpx=0xb5, abs=0xad, absx=0xbd, absy=0xb9, indx=0xa1, indy=0xb1),
    'LDX': dict(imm=0xa2, zp=0xa6, zpy=0xb6, abs=0xae, absy=0xbe),
    'LDY': dict(imm=0xa0, zp=0xa4, zpx=0xb4, abs=0xac, absx=0xbc),
    'LSR': dict(acc=0x4a, zp=0x46, zpx=0x56, abs=0x4e, absx=0x5e),
    'NOP': dict(imp=0xea),
    'ORA': dict(imm=0x09, zp=0x05, zpx=0x15, abs=0x0d, absx=0x1d, absy=0x19, indx=0x01, indy=0x11),
    'PHA': dict(imp=0x48), 'PHP': dict(imp=0x08), 'PLA': dict(imp=0x68), 'PLP': dict(imp=0x28),
    'ROL': dict(acc=0x2a, zp=0x26, zpx=0x36, abs=0x2e, absx=0x3e),
    'ROR': dict(acc=0x6a, zp=0x66, zpx=0x76, abs=0x6e, absx=0x7e),
    'RTI': dict(imp=0x40), 'RTS': dict(imp=0x60),
    'SBC': dict(imm=0xe9, zp=0xe5, zpx=0xf5, abs=0xed, absx=0xfd, absy=0xf9, indx=0xe1, indy=0xf1),
    'SEC': dict(imp=0x38), 'SED': dict(imp=0xf8), 'SEI': dict(imp=0x78),
    'STA': dict(zp=0x85, zpx=0x95, abs=0x8d, absx=0x9d, absy=0x99, indx=0x81, indy=0x91),
    'STX': dict(zp=0x86, zpy=0x96, abs=0x8e), 'STY': dict(zp=0x84, zpx=0x94, abs=0x8c),
    'TAX': dict(imp=0xaa), 'TAY': dict(imp=0xa8), 'TSX': dict(imp=0xba),
    'TXA': dict(imp=0x8a), 'TXS': dict(imp=0x9a), 'TYA': dict(imp=0x98),
}
ZP_FALLBACK = {'zp': 'abs', 'zpx': 'absx', 'zpy': 'absy'}
ABS_FALLBACK = {'abs': 'zp', 'absx': 'zpx', 'absy': 'zpy'}


class Asm:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.syms = {}

    def clean(self, line):
        out, quote = [], False
        for ch in line:
            if ch == '"':
                quote = not quote
            if ch == ';' and not quote:
                break
            out.append(ch)
        return ''.join(out).rstrip()

    def value(self, expr, final):
        expr = expr.strip()
        if expr.startswith('<'):
            return self.value(expr[1:], final) & 0xff
        if expr.startswith('>'):
            return (self.value(expr[1:], final) >> 8) & 0xff
        def sub(m):
            tok = m.group(0)
            if tok.startswith('$'):
                return str(int(tok[1:], 16))
            if tok.startswith('%'):
                return str(int(tok[1:], 2))
            if tok[0].isdigit():
                return tok
            if tok in self.syms:
                return str(self.syms[tok])
            if final:
                raise KeyError(tok)
            return '32768'
        text = re.sub(r'\$[0-9A-Fa-f]+|%[01]+|[A-Za-z_][A-Za-z_0-9]*|\d+', sub, expr)
        if text.startswith('<') or text.startswith('>'):
            return None
        return eval(text, {'__builtins__': {}}, {})

    def byte_expr(self, expr, final):
        expr = expr.strip()
        if expr.startswith('<'):
            return self.value(expr[1:], final) & 0xff
        if expr.startswith('>'):
            return (self.value(expr[1:], final) >> 8) & 0xff
        return self.value(expr, final) & 0xff

    def split_items(self, s):
        items, cur, quote = [], '', False
        for ch in s:
            if ch == '"':
                quote = not quote
            if ch == ',' and not quote:
                items.append(cur.strip())
                cur = ''
            else:
                cur += ch
        if cur.strip():
            items.append(cur.strip())
        return items

    def parse(self, line):
        """Return (label, body) for a cleaned line."""
        label = None
        m = re.match(r'^([A-Za-z_][A-Za-z_0-9]*):\s*(.*)$', line)
        if m:
            label, line = m.group(1), m.group(2)
        return label, line.strip()

    def operand_mode(self, mn, operand, final):
        """Return (mode, value) for an instruction."""
        if operand == '':
            if mn in ('ASL', 'LSR', 'ROL', 'ROR'):
                return 'acc', None
            return 'imp', None
        if operand == 'A' and mn in ('ASL', 'LSR', 'ROL', 'ROR'):
            return 'acc', None
        if mn in ('BCC', 'BCS', 'BEQ', 'BMI', 'BNE', 'BPL', 'BVC', 'BVS'):
            return 'rel', self.value(operand, final)
        if operand.startswith('#'):
            return 'imm', self.byte_expr(operand[1:], final)
        m = re.match(r'^\((.+),\s*[xX]\)$', operand)
        if m:
            return 'indx', self.byte_expr(m.group(1), final)
        m = re.match(r'^\((.+)\),\s*[yY]$', operand)
        if m:
            return 'indy', self.byte_expr(m.group(1), final)
        m = re.match(r'^\((.+)\)$', operand)
        if m:
            return 'ind', self.value(m.group(1), final)
        m = re.match(r'^(.+),\s*([xXyY])$', operand)
        if m:
            v = self.value(m.group(1), final)
            idx = m.group(2).lower()
            zp = v < 0x100 and not (final is False and False)
            return ('zp' + idx if zp else 'abs' + idx), v
        v = self.value(operand, final)
        return ('zp' if v < 0x100 else 'abs'), v

    def size_of(self, mn, operand, final):
        if mn in ('.DB', '.DW'):
            return None
        mode, _ = self.operand_mode(mn, operand, final)
        table = OPS[mn]
        if mode not in table:
            if mode in ZP_FALLBACK and ZP_FALLBACK[mode] in table:
                mode = ZP_FALLBACK[mode]
            elif mode in ABS_FALLBACK and ABS_FALLBACK[mode] in table:
                mode = ABS_FALLBACK[mode]
        return {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2,
                'indx': 2, 'indy': 2, 'rel': 2, 'abs': 3, 'absx': 3, 'absy': 3,
                'ind': 3}[mode]

    def assemble(self, final):
        """One pass. Returns (bytes-by-address dict, changed flag)."""
        addr = 0x8000
        emitted = {}
        changed = False
        for raw in self.lines:
            line = self.clean(raw).strip()
            if not line:
                continue
            m = re.match(r'^([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.+)$', line)
            if m:
                name, expr = m.group(1), m.group(2)
                try:
                    v = self.value(expr, final)
                except KeyError:
                    continue
                if self.syms.get(name) != v:
                    self.syms[name] = v
                    changed = True
                continue
            label, body = self.parse(line)
            if label is not None:
                if self.syms.get(label) != addr:
                    self.syms[label] = addr
                    changed = True
            if not body:
                continue
            parts = body.split(None, 1)
            mn = parts[0].upper()
            operand = parts[1] if len(parts) > 1 else ''
            if mn in ('.INDEX', '.MEM'):
                continue
            if mn == '.ORG':
                addr = self.value(operand, True)
                continue
            if mn == '.DB':
                for item in self.split_items(operand):
                    if item.startswith('"'):
                        for ch in item.strip('"'):
                            emitted[addr] = ord(ch)
                            addr += 1
                    else:
                        emitted[addr] = self.byte_expr(item, final)
                        addr += 1
                continue
            if mn == '.DW':
                for item in self.split_items(operand):
                    v = self.value(item, final)
                    emitted[addr] = v & 0xff
                    emitted[addr + 1] = (v >> 8) & 0xff
                    addr += 2
                continue
            if mn not in OPS:
                raise SystemExit('unknown mnemonic %r at %r' % (mn, raw))
            mode, v = self.operand_mode(mn, operand, final)
            table = OPS[mn]
            if mode not in table:
                if mode in ZP_FALLBACK and ZP_FALLBACK[mode] in table:
                    mode = ZP_FALLBACK[mode]
                elif mode in ABS_FALLBACK and ABS_FALLBACK[mode] in table:
                    mode = ABS_FALLBACK[mode]
            op = table[mode]
            if mode == 'rel':
                n = 2
                emitted[addr] = op
                emitted[addr + 1] = (v - (addr + 2)) & 0xff
            elif mode in ('imp', 'acc'):
                n = 1
                emitted[addr] = op
            elif mode in ('imm', 'zp', 'zpx', 'zpy', 'indx', 'indy'):
                n = 2
                emitted[addr] = op
                emitted[addr + 1] = v & 0xff
            else:
                n = 3
                emitted[addr] = op
                emitted[addr + 1] = v & 0xff
                emitted[addr + 2] = (v >> 8) & 0xff
            addr += n
        return emitted, changed


def build(path):
    text = open(path, encoding='utf-8').read()
    a = Asm(text)
    for _ in range(10):
        _, changed = a.assemble(final=False)
        if not changed:
            break
    emitted, _ = a.assemble(final=True)
    return a.syms, emitted


if __name__ == '__main__':
    syms, emitted = build(sys.argv[1])
    rom = open(sys.argv[2], 'rb').read()[16:16 + 0x8000]
    bad = [a for a in emitted if 0x8000 <= a < 0x10000 and emitted[a] != rom[a - 0x8000]]
    print('emitted', len(emitted), 'bytes; mismatches', len(bad))
    for a in sorted(bad)[:10]:
        print('  %04x asm=%02x rom=%02x' % (a, emitted[a], rom[a - 0x8000]))
    if '--syms' in sys.argv:
        for name, v in sorted(syms.items(), key=lambda kv: kv[1] if isinstance(kv[1], int) else 0):
            if isinstance(v, int) and v >= 0x8000:
                print('%04x %s' % (v, name))
