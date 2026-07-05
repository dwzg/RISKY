#!/usr/bin/env python3
"""
RISKY Assembler v3

Based on the v1 assembler (Copyright 2019 Dennis Witzig), rewritten
table-driven.  Differences from v1:

  - labels are resolved in ANY operand position (e.g. "in r0,#myfunc"
    works, which the compiler needs for function pointers and data
    addresses), not just as the first operand of a jump
  - robust immediate parsing (decimal, negative decimal, 0x hex)
  - errors report the source line instead of silently emitting "0"
  - usage: risky_asm.py input.asm [output.hex] [-q]

The pseudo instructions (call/ret, mov with special registers, 32 bit
register pairs, automatic immediate variants, push/pop immediate) and
all opcode encodings are identical to v1, so the output stays
compatible with the Logisim circuit.
"""

import sys
import os.path
import re

registers = {"r%d" % i: "%x" % i for i in range(16)}
registers.update({"*r%d" % i: "%x" % i for i in range(16)})

registers_32bit = {
    "er0": ["r0", "r1"],
    "er1": ["r2", "r3"],
    "er2": ["r4", "r5"],
    "er3": ["r6", "r7"],
    "er4": ["r8", "r9"],
    "er5": ["r10", "r11"],
    "er6": ["r12", "r13"],
    "er7": ["r14", "r15"]}

# even register of the pair encodes the 32 bit register
registers.update({er: registers[pair[0]] for er, pair in registers_32bit.items()})

registers_special = {
    "pc": ["ldpc", "jmpr"],
    "page": ["ldpage", "stopage"],
    "sp": ["ldsp", "stosp"],
    "state": ["ldstate", ""]}

ops_alu = {
    "add": "1", "sub": "2", "lmul": "3", "hmul": "4", "div": "5",
    "mod": "6", "shl": "7", "shr": "8", "and": "9", "or": "a",
    "xor": "b", "neg": "c1", "not": "c2",
    "add32": "c3", "sub32": "c4", "lmul32": "c5", "hmul32": "c6",
    "div32": "c7", "mod32": "c8", "neg32": "c9", "shl32": "ca",
    "shr32": "cb", "and32": "cc", "or32": "cd", "xor32": "ce",
    "not32": "cf",
    "addi": "e16", "subi": "e17", "lmuli": "e18", "hmuli": "e19",
    "divi": "e1a", "modi": "e1b", "negi": "e1c", "shli": "e1d",
    "shri": "e1e", "andi": "e1f", "ori": "e20", "xori": "e21",
    "noti": "e22",
    "cmp": "d3", "cmp32": "d4", "cmpi": "e15"}

ops_jump = {
    "jmpr": "e05", "jmzr": "e06", "jnzr": "e07", "jltr": "e08",
    "jnltr": "e09", "jeqr": "e0a", "jneqr": "e0b", "jgtr": "e0c",
    "jngtr": "e0d", "jmsr": "e0e", "jnsr": "e0f", "jmcr": "e10",
    "jncr": "e11", "jmbr": "e12", "jnbr": "e13", "jkbr": "e14",
    "jmp": "f000", "jmz": "f001", "jnz": "f002", "jlt": "f003",
    "jnlt": "f004", "jeq": "f005", "jneq": "f006", "jgt": "f007",
    "jngt": "f008", "jms": "f009", "jns": "f00a", "jmc": "f00b",
    "jnc": "f00c", "jmb": "f00d", "jnb": "f00e", "jkb": "f00f"}

# opcode encoding formats:
#   rrr   op r1,r2,r3        -> code r1 r2 r3 0000
#   rr    op r1,r2           -> code r1 r2 0000
#   r     op r1              -> code r1 0000
#   ri    op r1,#imm         -> code r1 imm16
#   ir    op *imm,r1         -> code r1 imm16      (stod)
#   rir   op *r1,#imm,r2     -> code r1 r2 imm16   (stoo)
#   rri   op r1,*r2,#imm     -> code r1 r2 imm16   (ldo)
#   i     op #imm            -> code imm16
#   n     op                 -> code 0000
encodings = {
    "nop":  ("n", ""),
    "mov":  ("rr", "c0"), "mov32": ("rr", "d2"),
    "stor": ("rr", "d0"), "ldr": ("rr", "d1"),
    "stoo": ("rir", "d5"), "ldo": ("rri", "d6"),
    "in":   ("ri", "e00"), "stod": ("ir", "e01"), "ldd": ("ri", "e02"),
    "stoi": ("ri", "e03"),
    "ldpc": ("r", "e04"), "ldpage": ("r", "e23"), "stopage": ("r", "e24"),
    "stosp": ("r", "e25"), "push": ("r", "e26"), "pop": ("r", "e27"),
    "ldstate": ("r", "e28"), "ldsp": ("r", "e29"),
    "pushi": ("i", "f010"), "popi": ("n", "f011")}

for op, code in ops_alu.items():
    if len(code) == 1:
        encodings[op] = ("rrr", code)
    elif op in ("cmp", "cmp32") or code.startswith("c") or code == "d3":
        encodings[op] = ("rr", code)
    else:
        encodings[op] = ("ri", code)
encodings["cmp"] = ("rr", "d3")
encodings["cmp32"] = ("rr", "d4")

for op, code in ops_jump.items():
    if op.endswith("r"):
        encodings[op] = ("r", code)
    else:
        encodings[op] = ("i", code)


def parseImmediate(value, source):
    value = value.lstrip("#*")
    try:
        if value.startswith("0x") or value.startswith("-0x"):
            number = int(value, 16)
        else:
            number = int(value)
    except ValueError:
        fail("invalid immediate value '" + value + "'", source)
    return "%04x" % (number & 0xffff)


def fail(message, source=None):
    if source is not None:
        print("Error: " + message + "  [" + source + "]")
    else:
        print("Error: " + message)
    exit(1)


def splitLine(line):
    return [arg for arg in re.split(r"[ \t]+|, ?|,", line) if arg != ""]


def normalize(line):
    """Lower case the mnemonic and any operand that is a register,
    special register or immediate; keep label operands case sensitive."""
    arg = splitLine(line)
    if line.endswith(":"):
        return line
    out = [arg[0].lower()]
    for operand in arg[1:]:
        low = operand.lower()
        if low.lstrip("#*") in registers or low in registers_special \
                or re.match(r"^[#*]?-?(0x[0-9a-f]+|[0-9]+)$", low):
            out.append(low)
        else:
            out.append(operand)
    if len(out) > 1:
        return out[0] + " " + ",".join(out[1:])
    return out[0]


def assemble(lines, quiet=True):
    # strip comments, whitespace, blank lines; normalize case
    content = []
    for line in lines:
        line = line.split(";")[0].strip()
        if line != "":
            content.append(normalize(line))

    # expand pseudo instructions (before label collection, so that
    # labels point at the expanded addresses)
    expanded = []
    for line in content:
        arg = splitLine(line)

        if arg[0] == "call":
            expanded.append("ldpc r0")
            expanded.append("addi r0,#4")
            expanded.append("push r0")
            expanded.append("jmp " + arg[1])
        elif arg[0] == "callr":            # call through register (function pointers)
            expanded.append("mov r1," + arg[1])
            expanded.append("ldpc r0")
            expanded.append("addi r0,#4")
            expanded.append("push r0")
            expanded.append("jmpr r1")
        elif arg[0] == "ret":
            expanded.append("pop r1")
            expanded.append("jmpr r1")
        elif arg[0] == "in" and arg[1] in registers_32bit:
            value = arg[2].lstrip("#*")
            number = int(value, 16) if "x" in value else int(value)
            data = "%08x" % (number & 0xffffffff)
            expanded.append("in " + registers_32bit[arg[1]][0] + ",#0x" + data[:4])
            expanded.append("in " + registers_32bit[arg[1]][1] + ",#0x" + data[4:])
        elif arg[0] == "mov" and arg[1] in registers_32bit:
            expanded.append("mov32 " + arg[1] + "," + arg[2])
        elif arg[0] == "mov" and len(arg) > 2 and arg[2] in registers_special:
            expanded.append(registers_special[arg[2]][0] + " " + arg[1])
        elif arg[0] == "mov" and arg[1] in registers_special:
            if registers_special[arg[1]][1] == "":
                fail("cannot write special register '" + arg[1] + "'", line)
            expanded.append(registers_special[arg[1]][1] + " " + arg[2])
        elif arg[0] in ops_alu and not arg[0].endswith("32") \
                and arg[1] in registers_32bit:
            expanded.append(arg[0] + "32 " + arg[1] + "," + arg[2])
        elif arg[0] in ops_alu and not arg[0].endswith("i") and len(arg) > 2 \
                and arg[-1] not in registers and len(arg) == 3:
            expanded.append(arg[0] + "i " + arg[1] + "," + arg[2])
        elif arg[0] in ops_jump and arg[1] in registers:
            base = arg[0][:-1] if arg[0].endswith("r") else arg[0]
            expanded.append(base + "r " + arg[1])
        elif arg[0] == "push" and arg[1] not in registers:
            expanded.append("pushi " + arg[1])
        elif arg[0] == "pop" and len(arg) == 1:
            expanded.append("popi")
        else:
            expanded.append(line)

    # collect labels (label address = index of the next real instruction)
    labels = {}
    instructions = []
    for line in expanded:
        if line.endswith(":"):
            label = line[:-1].strip()
            if label in labels:
                fail("duplicate label '" + label + "'")
            labels[label] = len(instructions)
        else:
            instructions.append(line)

    # substitute labels in any operand position
    resolved = []
    for line in instructions:
        arg = splitLine(line)
        newArgs = [arg[0]]
        for operand in arg[1:]:
            stripped = operand.lstrip("#*")
            if stripped in labels:
                prefix = operand[:len(operand) - len(stripped)]
                if prefix == "":
                    prefix = "*" if arg[0] in ops_jump else "#"
                newArgs.append(prefix + str(labels[stripped]))
            else:
                newArgs.append(operand)
        resolved.append(newArgs[0] + " " + ",".join(newArgs[1:]) if len(newArgs) > 1 else newArgs[0])

    # encode
    output = []
    for line in resolved:
        arg = splitLine(line)
        op = arg[0]
        if op not in encodings:
            fail("unknown instruction '" + op + "'", line)
        fmt, code = encodings[op]
        try:
            if fmt == "n":
                word = code + "0000" if code != "" else "0"
            elif fmt == "rrr":
                word = code + registers[arg[1]] + registers[arg[2]] + registers[arg[3]] + "0000"
            elif fmt == "rr":
                word = code + registers[arg[1]] + registers[arg[2]] + "0000"
            elif fmt == "r":
                word = code + registers[arg[1]] + "0000"
            elif fmt == "ri":
                word = code + registers[arg[1]] + parseImmediate(arg[2], line)
            elif fmt == "ir":
                word = code + registers[arg[2]] + parseImmediate(arg[1], line)
            elif fmt == "rir":
                word = code + registers[arg[1]] + registers[arg[3]] + parseImmediate(arg[2], line)
            elif fmt == "rri":
                word = code + registers[arg[1]] + registers[arg[2]] + parseImmediate(arg[3], line)
            elif fmt == "i":
                word = code + parseImmediate(arg[1], line)
        except (KeyError, IndexError):
            fail("bad operands", line)
        output.append(word)

    if not quiet:
        for address, line in enumerate(resolved):
            print("%4d  %-8s  %s" % (address, output[address], line))

    return output


def main():
    args = [a for a in sys.argv[1:] if a != "-q" and a != "-v"]
    verbose = "-v" in sys.argv

    if len(args) < 1:
        print("usage: risky_asm.py input.asm [output.hex] [-v]")
        exit(1)

    inputFileName = args[0]
    if len(args) > 1:
        outputFileName = args[1]
    else:
        outputFileName = os.path.splitext(inputFileName)[0] + ".hex"

    if not os.path.isfile(inputFileName):
        fail("input file does not exist: " + inputFileName)

    with open(inputFileName) as f:
        lines = f.read().splitlines()

    output = assemble(lines, quiet=not verbose)

    with open(outputFileName, "w") as f:
        f.write("v2.0 raw\n\n")
        for word in output:
            f.write(word + "\n")


if __name__ == "__main__":
    main()
