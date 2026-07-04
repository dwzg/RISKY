# RISKY Assembler (`risky_asm.py`)

Table-driven two-pass assembler. ~250 lines of Python. Output is Logisim
"v2.0 raw" hex format, compatible with the instruction ROM in `RISKY.circ`.

## Invocation

```
python3 risky_asm.py input.asm [output.hex] [-v]
```

| Flag | Effect |
|------|--------|
| `-v` | Verbose: print address, hex word, and source line for each instruction |

If no output file is given, `input.hex` is used.

## Pipeline

### Pass 0: Normalisation

Comments are stripped (`;` to end of line), blank lines removed. Each
remaining line is normalised: the mnemonic is lowercased; operands that
are registers, special registers, or immediate values are lowercased;
everything else (labels, identifiers) keeps its original case. This
means **labels are case-sensitive** but **mnemonics and register names
are case-insensitive**.

### Pass 1: Pseudo-op Expansion

Each line is split on whitespace and commas into an argument list.
Pseudo-ops are expanded into canonical instruction sequences:

| Pseudo-op | Expansion |
|-----------|-----------|
| `call target` | `ldpc r0` / `addi r0,#4` / `push r0` / `jmp target` |
| `callr reg` | `mov r1,reg` / `ldpc r0` / `addi r0,#4` / `push r0` / `jmpr r1` |
| `ret` | `pop r1` / `jmpr r1` |
| `mov erX,...` | `mov32 erX,...` |
| `mov ...,erX` | `mov32 ...,erX` |
| `mov reg,special` | `ldpc`/`ldpage`/`ldsp`/`ldstate` `reg` |
| `mov special,reg` | `stopage`/`stosp` `reg` |
| `alu erX,...` | `alu32 erX,...` (32-bit register pair → 32-bit op) |
| `alu reg,#imm` | `alui reg,#imm` (auto immediate variant) |
| `jmp reg` (etc.) | `jmpr reg` (auto register-indirect jump) |
| `cmp erX,...` | `cmp32 erX,...` |
| `push #imm` | `pushi #imm` |
| bare `pop` | `popi` |
| `in erX,#imm` | 32-bit immediate load split across two registers |
| `xor reg,#imm` | `xori reg,#imm` (auto immediate for bitwise ops) |

### Pass 2: Label Collection

Lines ending with `:` are label definitions. The label name (without
`:`) is mapped to the current instruction index (address). Labels must
be unique.

### Pass 3: Label Resolution

In every instruction, each operand is checked: if stripping `#`/`*`
prefixes yields a known label name, the operand is replaced with
`*address` (for jump targets) or `#address` (for data references).
**Labels can appear in any operand position**, not just as jump targets
— this is essential for function pointers and data addresses generated
by the compiler.

### Pass 4: Encoding

Each instruction is encoded as a 32-bit hex word. The encoding format is
determined by the opcode table:

| Format | Description | Example |
|--------|-------------|---------|
| `n` | No operands | `nop`, `popi` |
| `r` | One register | `push r0`, `ldpc r0` |
| `rr` | Two registers | `mov r0,r1`, `cmp r0,r1` |
| `rrr` | Three registers | `add r0,r1,r2` |
| `ri` | Register + immediate | `in r0,#1234`, `addi r0,#5` |
| `ir` | Immediate + register | `stod *addr,r0` |
| `rir` | Register + immediate + register | `stoo *r0,#off,r1` |
| `rri` | Register + register + immediate | `ldo r0,*r1,#off` |
| `i` | Immediate only | `jmp *addr`, `pushi #val` |

**Immediate parsing:** supports decimal, negative decimal, and hex
(`0x` prefix). Values are masked to 16 bits.

**Word format:** the 32-bit encoding is `[opcode nibbles][reg fields][16-bit immediate]`.

## Opcode tables

**ALU (ternary, format `rrr` unless noted):**
`add sub lmul hmul div mod shl shr and or xor neg not`
Plus `32` suffix variants (binary, format `rr`): `add32 sub32 lmul32 hmul32 div32 mod32 neg32 shl32 shr32 and32 or32 xor32 not32`
Plus `i` suffix variants (format `ri`): `addi subi lmuli hmuli divi modi negi shli shri andi ori xori noti`

**Register ops (format `rr`):** `mov mov32`
**Register ops (format `r`):** `ldpc ldpage stopage stosp ldstate ldsp`
**Register ops (format `ri`):** `in`

**Memory ops:** `stor ldr` (`rr`), `stoo` (`rir`), `ldo` (`rri`),
`stod` (`ir`), `ldd` (`ri`), `stoi` (`ri`)

**Stack ops:** `push` (`r`), `pop` (`r`), `pushi` (`i`), `popi` (`n`)

**Comparison:** `cmp cmp32` (`rr`), `cmpi` (`ri`)

**Jumps (immediate, format `i`):** `jmp jmz jnz jlt jnlt jeq jneq jgt jngt jms jns jmc jnc jmb jnb jkb`
**Jumps (register, format `r`):** `jmpr jmzr jnzr jltr jnltr jeqr jneqr jgtr jngtr jmsr jnsr jmcr jncr jmbr jnbr jkbr`

## Register encoding

16 general-purpose registers: `r0`–`r15` → nibbles `0`–`f`.
Dereference prefix `*rN` encodes the same as `rN` (the dereference is
implicit in the opcode, not the register field).

32-bit register pairs (even register holds high word):
`er0`=`r0:r1`, `er1`=`r2:r3`, … `er7`=`r14:r15`.
Only the even register number is encoded.

Special registers accessed via pseudo-op expansion:
`pc` → `ldpc`/`jmpr`, `page` → `ldpage`/`stopage`,
`sp` → `ldsp`/`stosp`, `state` → `ldstate` (read-only).
