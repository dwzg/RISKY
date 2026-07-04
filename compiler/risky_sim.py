#!/usr/bin/env python3
"""
RISKY ISA simulator

Executes .hex files produced by risky_asm.py so that compiler output can
be tested without clicking through Logisim.  The machine model follows
"RISKY DOKU.txt" and the assembler encodings:

  - 16 bit words, word addressed memory
  - 16 general purpose registers r0..r15 (even/odd pairs form er0..er7)
  - separate instruction memory (the .hex file)
  - paged data memory: the page register selects the target of ALL data
    memory operations, including push/pop (verified against RISKY.circ:
    SP and PAGE both live in RAMREG and feed one shared RAM address bus)
      pages 0x0..0x9: ten independent 64K word RAMs
      page  0xa:      terminal; a write to address 0x0 prints chr(value),
                      writes elsewhere are dropped, reads return 0
      page  0xb:      read only data ROM (preloadable input buffer)
  - push stores at sp, then decrements; pop increments, then loads
  - cmp/cmpi/cmp32 set the lt/eq/neq/gt flags (signed compare); ALU ops
    set zero/sign/carry

The simulator halts on an unconditional jump to its own address (the
idiom the compiler emits after main returns) or when the cycle limit is
reached.
"""

import sys

MASK16 = 0xffff
MASK32 = 0xffffffff


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


def signed32(value):
    return value - 0x100000000 if value & 0x80000000 else value


class Halt(Exception):
    pass


class Simulator:
    def __init__(self, program, inputData=""):
        self.program = program           # list of 32 bit instruction words
        self.regs = [0] * 16
        self.pc = 0
        self.sp = 0
        self.page = 0
        self.pages = {}                  # page number -> 64K word list
        self.inputData = [ord(c) for c in inputData]
        self.output = []
        self.cycles = 0
        # flags
        self.zero = False
        self.sign = False
        self.carry = False
        self.borrow = False
        self.lt = False
        self.eq = False
        self.gt = False

    # ------------------------------------------------------------- memory

    def dataPage(self, page):
        if page not in self.pages:
            self.pages[page] = [0] * 0x10000
        return self.pages[page]

    def store(self, address, value):
        address &= MASK16
        if self.page == 0xa:
            if address == 0x0:           # terminal register
                self.output.append(chr(value & 0xff))
            return                       # other addresses: no memory here
        if self.page == 0xb:
            return                       # data ROM is read only
        if self.page <= 0x9:
            self.dataPage(self.page)[address] = value & MASK16

    def load(self, address):
        address &= MASK16
        if self.page == 0xa:
            return 0                     # terminal is write only
        if self.page == 0xb:             # data ROM
            if address < len(self.inputData):
                return self.inputData[address] & MASK16
            return 0
        if self.page <= 0x9:
            return self.dataPage(self.page)[address]
        return 0

    def push(self, value):
        self.store(self.sp, value)
        self.sp = (self.sp - 1) & MASK16

    def pop(self):
        self.sp = (self.sp + 1) & MASK16
        return self.load(self.sp)

    # -------------------------------------------------------------- flags

    def stateWord(self):
        state = 0
        if self.zero:   state |= 0x01
        if self.carry:  state |= 0x02
        if self.lt:     state |= 0x04
        if self.sign:   state |= 0x08
        if self.eq:     state |= 0x10
        if not self.eq: state |= 0x20
        if self.gt:     state |= 0x40
        if self.borrow: state |= 0x80
        return state

    def setResultFlags(self, result, carry=False, borrow=False):
        self.zero = (result & MASK16) == 0
        self.sign = bool(result & 0x8000)
        self.carry = carry
        self.borrow = borrow

    def compare(self, a, b):
        self.lt = a < b
        self.eq = a == b
        self.gt = a > b
        self.zero = a == b

    # ---------------------------------------------------------------- alu

    def alu(self, op, a, b):
        if op == 0x1:                                    # add
            result = a + b
            self.setResultFlags(result, carry=result > MASK16)
        elif op == 0x2:                                  # sub
            result = a - b
            self.setResultFlags(result, borrow=result < 0)
        elif op == 0x3:                                  # lmul
            result = a * b
            self.setResultFlags(result)
        elif op == 0x4:                                  # hmul
            result = (a * b) >> 16
            self.setResultFlags(result)
        elif op == 0x5:                                  # div (unsigned)
            result = 0 if b == 0 else (a // b) & MASK16
            self.setResultFlags(result)
        elif op == 0x6:                                  # mod (unsigned)
            result = 0 if b == 0 else (a % b) & MASK16
            self.setResultFlags(result)
        elif op == 0x7:                                  # shl
            result = a << (b & 0x1f)
            self.setResultFlags(result, carry=result > MASK16)
        elif op == 0x8:                                  # shr (logical)
            result = a >> (b & 0x1f)
            self.setResultFlags(result)
        elif op == 0x9:                                  # and
            result = a & b
            self.setResultFlags(result)
        elif op == 0xa:                                  # or
            result = a | b
            self.setResultFlags(result)
        elif op == 0xb:                                  # xor
            result = a ^ b
            self.setResultFlags(result)
        else:
            raise Halt("unknown ALU op %x at pc=%d" % (op, self.pc))
        return result & MASK16

    def alu32(self, op, a, b):
        # op is the low nibble of 0xC3..0xCE
        if op == 0x3:   result = a + b
        elif op == 0x4: result = a - b
        elif op == 0x5: result = (a * b) & MASK32
        elif op == 0x6: result = (a * b) >> 32
        elif op == 0x7: result = 0 if b == 0 else (a // b) & MASK32
        elif op == 0x8:
            result = 0 if b == 0 else (a % b) & MASK32
        elif op == 0xa: result = a << (b & 0x3f)
        elif op == 0xb: result = a >> (b & 0x3f)
        elif op == 0xc: result = a & b
        elif op == 0xd: result = a | b
        elif op == 0xe: result = a ^ b
        else:
            raise Halt("unknown 32 bit ALU op %x at pc=%d" % (op, self.pc))
        self.zero = (result & MASK32) == 0
        self.sign = bool(result & 0x80000000)
        return result & MASK32

    def getPair(self, n):
        n &= 0xe
        return (self.regs[n] << 16) | self.regs[n + 1]

    def setPair(self, n, value):
        n &= 0xe
        self.regs[n] = (value >> 16) & MASK16
        self.regs[n + 1] = value & MASK16

    # --------------------------------------------------------------- step

    def jumpCondition(self, cond):
        # cond: index of the jump op (0=jmp, 1=jmz, 2=jnz, 3=jlt, 4=jnlt,
        # 5=jeq, 6=jneq, 7=jgt, 8=jngt, 9=jms, 10=jns, 11=jmc, 12=jnc,
        # 13=jmb, 14=jnb, 15=jkb)
        flags = [True, self.zero, not self.zero,
                 self.lt, not self.lt,
                 self.eq, not self.eq,
                 self.gt, not self.gt,
                 self.sign, not self.sign,
                 self.carry, not self.carry,
                 self.borrow, not self.borrow,
                 False]                  # no keyboard in the circuit
        return flags[cond]

    def step(self):
        if self.pc >= len(self.program) or self.pc < 0:
            raise Halt("pc out of range: %d" % self.pc)
        word = self.program[self.pc]
        self.cycles += 1

        op = (word >> 28) & 0xf
        n1 = (word >> 24) & 0xf
        n2 = (word >> 20) & 0xf
        n3 = (word >> 16) & 0xf
        imm = word & MASK16
        nextPc = self.pc + 1

        if op == 0x0:                                    # nop
            pass
        elif 0x1 <= op <= 0xb:                           # 3 operand ALU
            self.regs[n1] = self.alu(op, self.regs[n2], self.regs[n3])
        elif op == 0xc:
            if n1 == 0x0:                                # mov
                self.regs[n2] = self.regs[n3]
            elif n1 == 0x1:                              # neg
                result = (-signed16(self.regs[n3])) & MASK16
                self.setResultFlags(result)
                self.regs[n2] = result
            elif n1 == 0x2:                              # not
                result = (~self.regs[n3]) & MASK16
                self.setResultFlags(result)
                self.regs[n2] = result
            elif n1 == 0x9:                              # neg32
                self.setPair(n2, (-signed32(self.getPair(n3))) & MASK32)
            elif n1 == 0xf:                              # not32
                self.setPair(n2, (~self.getPair(n3)) & MASK32)
            else:                                        # 32 bit ALU
                self.setPair(n2, self.alu32(n1, self.getPair(n2), self.getPair(n3)))
        elif op == 0xd:
            if n1 == 0x0:                                # stor *rA,rB
                self.store(self.regs[n2], self.regs[n3])
            elif n1 == 0x1:                              # ldr rA,*rB
                self.regs[n2] = self.load(self.regs[n3])
            elif n1 == 0x2:                              # mov32
                self.setPair(n2, self.getPair(n3))
            elif n1 == 0x3:                              # cmp
                self.compare(signed16(self.regs[n2]), signed16(self.regs[n3]))
            elif n1 == 0x4:                              # cmp32
                self.compare(signed32(self.getPair(n2)), signed32(self.getPair(n3)))
            elif n1 == 0x5:                              # stoo *rA,#imm,rB
                self.store(self.regs[n2] + imm, self.regs[n3])
            elif n1 == 0x6:                              # ldo rA,*rB,#imm
                self.regs[n2] = self.load(self.regs[n3] + imm)
            else:
                raise Halt("unknown 0xD opcode at pc=%d" % self.pc)
        elif op == 0xe:
            sub = (word >> 20) & 0xff                    # two nibbles
            reg = n3
            if sub == 0x00:                              # in rX,#imm
                self.regs[reg] = imm
            elif sub == 0x01:                            # stod *imm,rX
                self.store(imm, self.regs[reg])
            elif sub == 0x02:                            # ldd rX,*imm
                self.regs[reg] = self.load(imm)
            elif sub == 0x03:                            # stoi *rX,#imm
                self.store(self.regs[reg], imm)
            elif sub == 0x04:                            # ldpc rX
                self.regs[reg] = self.pc & MASK16
            elif 0x05 <= sub <= 0x14:                    # register jumps
                if self.jumpCondition(sub - 0x05):
                    nextPc = self.regs[reg]
            elif sub == 0x15:                            # cmpi
                self.compare(signed16(self.regs[reg]), signed16(imm))
            elif 0x16 <= sub <= 0x22:                    # ALU with immediate
                # map: 0x16..0x1b -> add..mod, 0x1c neg, 0x1d shl, 0x1e shr,
                #      0x1f and, 0x20 or, 0x21 xor, 0x22 not
                if sub <= 0x1b:                          # addi..modi
                    self.regs[reg] = self.alu(sub - 0x15, self.regs[reg], imm)
                elif sub == 0x1c:                        # negi
                    self.regs[reg] = (-signed16(imm)) & MASK16
                elif sub == 0x1d:
                    self.regs[reg] = self.alu(0x7, self.regs[reg], imm)
                elif sub == 0x1e:
                    self.regs[reg] = self.alu(0x8, self.regs[reg], imm)
                elif sub == 0x1f:
                    self.regs[reg] = self.alu(0x9, self.regs[reg], imm)
                elif sub == 0x20:
                    self.regs[reg] = self.alu(0xa, self.regs[reg], imm)
                elif sub == 0x21:
                    self.regs[reg] = self.alu(0xb, self.regs[reg], imm)
                elif sub == 0x22:                        # noti
                    self.regs[reg] = (~imm) & MASK16
            elif sub == 0x23:                            # ldpage rX
                self.regs[reg] = self.page
            elif sub == 0x24:                            # stopage rX
                self.page = self.regs[reg]
            elif sub == 0x25:                            # stosp rX
                self.sp = self.regs[reg]
            elif sub == 0x26:                            # push rX
                self.push(self.regs[reg])
            elif sub == 0x27:                            # pop rX
                self.regs[reg] = self.pop()
            elif sub == 0x28:                            # ldstate rX
                self.regs[reg] = self.stateWord()
            elif sub == 0x29:                            # ldsp rX
                self.regs[reg] = self.sp
            else:
                raise Halt("unknown 0xE opcode %02x at pc=%d" % (sub, self.pc))
        elif op == 0xf:
            sub = (word >> 16) & 0xfff
            if sub <= 0x00f:                             # immediate jumps
                if self.jumpCondition(sub):
                    if sub == 0 and imm == self.pc:
                        raise Halt("halted (jump to self) at pc=%d" % self.pc)
                    nextPc = imm
            elif sub == 0x010:                           # pushi
                self.push(imm)
            elif sub == 0x011:                           # popi
                self.sp = (self.sp + 1) & MASK16
            else:
                raise Halt("unknown 0xF opcode at pc=%d" % self.pc)
        else:
            raise Halt("unknown opcode at pc=%d" % self.pc)

        self.pc = nextPc

    def run(self, maxCycles=10000000, trace=False):
        reason = "cycle limit reached (%d)" % maxCycles
        try:
            while self.cycles < maxCycles:
                if trace:
                    print("pc=%04d %08x  r0=%04x r1=%04x r15=%04x sp=%04x"
                          % (self.pc, self.program[self.pc], self.regs[0],
                             self.regs[1], self.regs[15], self.sp),
                          file=sys.stderr)
                self.step()
        except Halt as halt:
            reason = str(halt)
        return reason


def loadHex(fileName):
    program = []
    with open(fileName) as f:
        for line in f:
            line = line.strip()
            if line == "" or line.startswith("v2.0"):
                continue
            for word in line.split():
                program.append(int(word, 16) & MASK32)
    return program


def runProgram(program, inputData="", maxCycles=10000000, trace=False):
    sim = Simulator(program, inputData)
    reason = sim.run(maxCycles, trace)
    return sim, reason


def main():
    args = sys.argv[1:]
    trace = "--trace" in args
    verbose = "-v" in args
    args = [a for a in args if a not in ("--trace", "-v")]

    inputData = ""
    maxCycles = 10000000
    positional = []
    i = 0
    while i < len(args):
        if args[i] == "-i":
            with open(args[i + 1]) as f:
                inputData = f.read()
            i += 2
        elif args[i] == "--max-cycles":
            maxCycles = int(args[i + 1])
            i += 2
        else:
            positional.append(args[i])
            i += 1

    if len(positional) != 1:
        print("usage: risky_sim.py prog.hex [-i inputfile] [--max-cycles N] [--trace] [-v]")
        exit(1)

    program = loadHex(positional[0])
    sim, reason = runProgram(program, inputData, maxCycles, trace)
    sys.stdout.write("".join(sim.output))
    sys.stdout.flush()

    if verbose:
        print("\n--- " + reason + " after %d cycles" % sim.cycles, file=sys.stderr)
        for i in range(0, 16, 4):
            print("  " + "  ".join("r%-2d=%04x" % (j, sim.regs[j])
                                   for j in range(i, i + 4)), file=sys.stderr)
        print("  sp=%04x page=%x pc=%d" % (sim.sp, sim.page, sim.pc), file=sys.stderr)


if __name__ == "__main__":
    main()
