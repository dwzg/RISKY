# RISKY C Compiler (`risky_c.py`)

A nearly C89-compliant compiler targeting the RISKY CPU. Single-pass:
regex lexer → recursive-descent parser → AST classes with `generate()`
methods emitting RISKY assembly text. No IR or optimisation passes.
~3100 lines of Python.

## Invocation

```
python3 risky_c.py input.c [-o output.asm] [-I includedir] [--ast] [--keep-dead]
```

| Flag | Effect |
|------|--------|
| `-o file.asm` | Output assembly file (default: `input.asm`) |
| `-I dir` | Add include search directory |
| `--ast` | Dump the AST to stdout (debug) |
| `--keep-dead` | Skip dead function elimination |

## Pipeline

### 1. Preprocessor (`class Preprocessor`)

Runs before lexing. Produces a flat token stream.

- **Comment stripping:** `/* */` (nestable), `//` (to end of line). Handles
  string/character literal boundaries so comment delimiters inside strings
  are ignored.
- **Line continuation:** backslash-newline joins physical lines (line
  numbers are tracked through the filler).
- **`#include "..."` / `#include <...>`:** recursively processes the
  included file. `"..."` searches the including file's directory first,
  then `-I` dirs, then `compiler/lib/`. `<...>` searches `-I` dirs then
  `compiler/lib/`.
- **`#define`:** object-like (`#define FOO bar`) and function-like
  (`#define MAX(a,b) ((a)>(b)?(a):(b))`). Expansion is applied
  recursively with hidden-name guards against infinite recursion.
- **`#undef`:** removes a macro.
- **`#if` / `#elif` / `#else` / `#endif`:** constant-expression
  evaluation with `defined()`. Identifiers not known as macros evaluate
  to 0 (C89 semantics).
- **`#ifdef` / `#ifndef`:** shorthand for `#if defined(X)`.
- **`#error`:** emits a compile-time error.
- **`#pragma`:** silently ignored.
- **Predefined:** `__RISKY__` expands to `1`.

### 2. Lexer (`lexLine()`)

Regex-based tokeniser. One regex alternation covering all token types;
longest match wins. Produces `Token(type, name, line, value)` objects.

Token types: keywords (recognised by name), punctuators (operators,
braces), `IDENTIFIER`, `CONSTANT` (integer or character literal, value
stored as Python int), `STRING` (decoded, escape sequences resolved).

### 3. Parser (`class Parser`)

Recursive-descent, matches the grammar in `docs/c-grammar.txt`.

**Expression parsing:** precedence-climbing via `parseBinaryExpression(level)`.
Operator levels: `||` < `&&` < `|` < `^` < `&` < `==`/`!=` < `<`/`>`/`<=`/`>=`
< `<<`/`>>` < `+`/`-` < `*`/`/`/`%`.

**Declarator parsing:** handles the C declarator syntax (pointers, arrays,
functions, parenthesised inner declarators). Builds types bottom-up:
base type → pointer wrappers → array/function suffix wrappers.

**`__naked`:** recognised in `parseDeclarationSpecifiers()`, stored on the
Parser instance as `nakedFlag`, passed to `FunctionDefinition`.

### 4. Code Generation (`class Codegen` + AST `generate(scope)` methods)

Each AST node class has a `generate(scope)` method returning RISKY
assembly text as a Python string. `Scope` objects track local variable
offsets (word offsets relative to `r15`).

**Global allocation:** `Codegen.allocGlobal()` bumps a cursor from address
0 upward. Global initializers are recorded in `Codegen.globalInits` and
emitted as `stod` instructions in the startup code. String literals are
deduplicated via `internString()` and placed after all globals.

**Startup code** (emitted by `Program.generate()`):
```asm
in r0,#0
mov page,r0         ; select page 0
in r0,#0xffff
stosp r0            ; sp = 0xffff
... global init stores ...
... string init stores ...
call main
__halt:
jmp __halt          ; simulator detects this as termination
```

**Function prologue/epilogue** (unless `__naked`):
```asm
funcname:
push r15
mov r15,sp
[subi sp,#N]        ; reserve locals
... body ...
in r0,#0            ; return value for void
mov sp,r15
pop r15
ret
```

**Runtime helpers:** `__sdiv` and `__smod` are emitted at the end of the
program when the `/` or `%` operators are used. They wrap the hardware's
unsigned `div`/`mod` with sign handling (abs of both operands, toggle
sign flag, unsigned op, conditionally negate result).

**Dead function elimination:** `eliminateDeadFunctions()` traces
reachability from startup code + `main`, strips unreferenced functions.

## Supported C dialect

**Types:** `void`, `char`, `short`, `int`, `long`, `signed`, `unsigned`,
pointers (any depth), arrays (multidimensional, `[]` with size from
initializer), `struct`, `union` (nested, by-value assignment and
parameters), `enum`, `typedef`, function pointers.

**Statements:** `if`/`else`, `while`, `do`/`while`, `for`, `switch`/`case`/
`default` (with fallthrough), `break`, `continue`, `goto`/labels, `return`,
compound `{}`, empty `;`.

**Expressions:** all C89 operators including compound assignment (`+=`,
`-=`, etc.), prefix/postfix `++`/`--` (pointer-scaled), ternary `?:`,
comma, `sizeof` (type and expression), casts, short-circuit `&&`/`||`,
pointer arithmetic with `p - q`, `a[i]` ⇔ `i[a]`, adjacent string
literal concatenation.

**Initializers:** brace-enclosed lists (nested for structs/arrays),
string literals for `char` arrays, `char s[] = "..."` with size
inference, global initializers must be constant expressions (or
addresses of globals/functions).

**Preprocessor:** `#include`, `#define` (object/function-like),
`#undef`, `#if`/`#elif`/`#else`/`#endif` (with `defined()` operator,
constant expressions, `&&`/`||`/`!`), `#ifdef`/`#ifndef`, `#error`.

**Extensions:**
- `__asm__("...")` — inline assembler. Multiple adjacent string literals
  are concatenated. C escape sequences (`\n`, `\t`) are decoded.
- `__naked` — function attribute. Skips prologue/epilogue. Body must be
  pure `__asm__`. Used for context-switch routines.
- `__RISKY__` — predefined macro, expands to `1`.

## Deviations from C89

| Area | Behaviour |
|------|-----------|
| Type sizes | `sizeof` everything = 1 word. `char` = `short` = `int` = `long` = `T*` = 16 bits |
| `long` | 16-bit; warning emitted |
| `float`/`double` | Compile error (no FPU). Use `softfloat.h` for binary16 in software |
| `>>` on signed | Emits `shr` (logical shift). Gives wrong results for negative values. Use `/` instead |
| `/` and `%` | Hardware `div`/`mod` are unsigned. Compiler generates `__sdiv`/`__smod` sign-handling wrappers |
| `>>` in preprocessor | Python's arithmetic shift; may differ from hardware for compile-time expressions |
| Bit fields | Not supported (compile error) |
| K&R function syntax | Not supported |
| Returning structs | Not supported (compile error) |
| `unsigned` arithmetic | Treated as signed in most operations |
| Linker | None — single translation unit. Use `#include` for multi-file programs |
| `sizeof` applied to `char`/`short`/`int` | Always 1; `mem*`/`malloc` sizes are in words |
