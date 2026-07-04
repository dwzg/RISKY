# RISKY ISA Simulator (`risky_sim.py`)

Instruction-level interpreter for the RISKY CPU. Loads `.hex` files
produced by the assembler and executes them cycle by cycle. ~350 lines.

## Invocation

```
python3 risky_sim.py prog.hex [-i inputfile] [--max-cycles N] [--trace] [-v]
```

| Flag | Effect |
|------|--------|
| `-i file` | Preload input file as page 0xb ROM (stdin). Plain text or "v2.0 raw" hex |
| `--max-cycles N` | Maximum cycles before forced stop (default: 10 000 000) |
| `--trace` | Log every instruction to stderr (pc, opcode, registers) |
| `-v` | Dump register state on exit |

## Machine model

- 16-bit word-addressed memory, 16 general-purpose registers (`r0`–`r15`)
- Separate instruction memory (loaded from `.hex`)
- Paged data memory: pages 0–9 are 64K×16 RAM banks, page 0xa is the
  terminal, page 0xb is the ROM input buffer
- `sp` and `page` both feed the same RAM address bus — push/pop go
  through the paged RAM. Must have page 0 selected during stack operations
- `push(value)`: stores at `sp`, then `sp--`
- `pop()`: `sp++`, then loads from `sp` (full-ascending stack)
- Halts on jump-to-self detection (`jmp *ADDR` where ADDR equals the
  current PC), matching the compiler's `__halt: jmp __halt` idiom

## I/O model

**Page 0xa (terminal, write-only):** a write to address 0 prints
`chr(value & 0xff)` to stdout. Writes to other addresses are silently
dropped. Reads return 0.

**Page 0xb (input ROM, read-only):** preloaded from the `-i` file. Each
character becomes one ROM word (low byte = ASCII code, high byte = 0).
Reads at addresses beyond the input length return 0. Writes are silently
dropped.

## Instruction execution

The 32-bit instruction word is decoded into a 4-bit opcode field, three
4-bit register/immediate-selector fields, and a 16-bit immediate.

### ALU operations (ops 0x1–0xB)

Three-operand: `dest = alu(src1, src2)`. The `alu()` method implements:

| Op | Operation | Notes |
|----|-----------|-------|
| 0x1 ADD | `a + b` | Sets carry flag |
| 0x2 SUB | `a - b` | Sets borrow flag |
| 0x3 LMUL | `a * b` (low 16) | |
| 0x4 HMUL | `a * b` (high 16) | |
| 0x5 DIV | `a / b` | **Unsigned** |
| 0x6 MOD | `a % b` | **Unsigned** |
| 0x7 SHL | `a << b` | |
| 0x8 SHR | `a >> b` | **Logical** (unsigned) |
| 0x9 AND | `a & b` | |
| 0xA OR  | `a \| b` | |
| 0xB XOR | `a ^ b` | |

Division by zero returns 0.

### 0xC-family operations (binary register ops)

`MOV` (0xC0), `NEG` (0xC1), `NOT` (0xC2), 32-bit ALU ops (0xC3–0xCF).
32-bit ops use even/odd register pairs. `NEG` and `NOT` are signed and
bitwise respectively.

### 0xD-family (memory + comparison)

`STOR` (0xD0), `LDR` (0xD1), `MOV32` (0xD2), `CMPR` (0xD3), `CMPR32`
(0xD4), `STOO` (0xD5, store with offset), `LDO` (0xD6, load with offset).

`CMPR` sets `lt`, `eq`, `gt`, `zero` flags via signed comparison of the
two operands. **Does not set the `sign` flag** (this is a known
difference from what `jns` after `cmp` would need).

### 0xE-family (immediate ALU, register ops, stack, control)

`IN` (0xE00, load immediate), `STOD`/`LDD` (store/load at absolute
address), `STOI` (store immediate indirect), `LDPC` (load program
counter), register-indirect jumps (0xE05–0xE14), `CMPI` (compare with
immediate), immediate ALU ops (0xE16–0xE22), `LDPAGE`/`STOPAGE`,
`STOSP`, `PUSH`/`POP`, `LDSTATE`, `LDSP`.

### 0xF-family (immediate jumps, stack)

Immediate jumps (0xF000–0xF00F), `PUSHI` (0xF010), `POPI` (0xF011).
Jump condition testing: checks the corresponding flag (zero, sign,
carry, borrow, lt, eq, gt) against the jump opcode index.

## State flags

Status register modelled as separate boolean fields, accessible via
`LDSTATE`. Flag bit layout (matching the compiler's usage):

| Bit | Flag | Set by |
|-----|------|--------|
| 0x01 | zero | ALU result = 0, CMP equality |
| 0x02 | carry | ADD overflow |
| 0x04 | lt | CMP: a < b (signed) |
| 0x08 | sign | ALU result bit 15 |
| 0x10 | eq | CMP: a == b |
| 0x20 | neq | CMP: a != b (complement of eq) |
| 0x40 | gt | CMP: a > b (signed) |
| 0x80 | borrow | SUB underflow |

## Known simulator/hardware differences

| Area | Simulator | Hardware (Logisim) |
|------|-----------|-------------------|
| DIV/MOD | Unsigned (matches hardware) | Unsigned |
| SHR | Logical (matches hardware) | Logical |
| CMP sign flag | Not set | Unknown |
| Keyboard | No keyboard component | No keyboard component |
| Terminal dimensions | Unlimited output buffer | 32 cols × 8 rows TTY |
