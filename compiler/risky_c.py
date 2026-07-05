#!/usr/bin/env python3
"""
RISKY C compiler v3

A small C89 compiler for the RISKY CPU, completing the v2 compiler.

Supported:
  - types: void, char, short, int, long, signed/unsigned (all integer
    types are one 16 bit word on this word addressed machine; long is
    16 bit too, a warning is emitted), pointers (any depth), arrays
    (incl. multidimensional), struct/union (incl. nested, by-value
    assignment and by-value parameters), enum, typedef, function
    pointers
  - all C89 statements: if/else, while, do/while, for, switch/case/
    default, break, continue, goto/labels, return, compound, empty
  - all C89 operators incl. compound assignment, ++/--, ?:, comma,
    sizeof, casts, short circuit &&/||, pointer arithmetic
  - string literals, character constants, hex/octal/decimal constants
  - global variables and static locals with constant initializers,
    local initializers incl. brace initializer lists and char[] = "..."
  - preprocessor: #include "..."/<...>, #define (object- and function-
    like), #undef, #ifdef, #ifndef, #else, #endif, #if/#elif with
    constant expressions and defined(), comments (/* */ and //)
  - __asm__("...") statements for inline assembler
  - varargs functions can be written portably via  int *ap = &last + 1;

Not supported (compile time error):
  - float/double (the CPU has no FPU)
  - returning structs/unions by value
  - bit fields

ABI (same as compiler v1/v2):
  r0 = return value / expression accumulator, r1/r2/r3 = scratch,
  r15 = frame pointer.  Arguments are pushed right to left, the caller
  pops them.  Parameter i starts at r15+3, locals grow down from r15-0.
  All memory (globals from address 0 up, heap from 0x8000 up, stack
  from 0xffff down) lives in page 0.  The generated program initializes
  page, sp, globals and strings, calls main and halts on a jump-to-self.

Usage: risky_c.py input.c [-o output.asm] [-I includedir] [--ast] [--keep-dead]
"""

import sys
import os
import re

############################### DIAGNOSTICS ###############################

currentSourceName = "<input>"


def fail(error, line=None):
    location = currentSourceName + (":" + str(line) if line else "")
    sys.stderr.write("%s: error: %s\n" % (location, error))
    sys.exit(1)


def warn(warning, line=None):
    location = currentSourceName + (":" + str(line) if line else "")
    sys.stderr.write("%s: warning: %s\n" % (location, warning))


############################### LEXER ###############################

keywords = {
    "auto", "break", "case", "char", "const", "continue", "default",
    "do", "double", "else", "enum", "extern", "float", "for", "goto",
    "if", "int", "long", "register", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "__asm__", "__naked"}

punctuators = [
    "<<=", ">>=", "...",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=",
    "{", "}", "(", ")", "[", "]", ";", ",", ":", "?", "~",
    "+", "-", "*", "/", "%", "&", "|", "^", "!", "<", ">", "=", "."]

tokenPattern = re.compile(
    r'"(?:\\.|[^"\\])*"'                       # string literal
    r"|'(?:\\.|[^'\\])+'"                      # character constant
    # float literal: digit sequences containing . or e/E, optional f/F suffix
    r"|\d+\.\d*([eE][+-]?\d+)?[fF]?"           # 3.14f, 3.f
    r"|\.\d+([eE][+-]?\d+)?[fF]?"              # .5f
    r"|\d+[eE][+-]?\d+[fF]?"                   # 1e10f (no dot)
    r"|0[xX][0-9a-fA-F]+\.[0-9a-fA-F]*[pP][+-]?\d+[fF]?"  # hex float
    r"|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*"    # numeric constant
    r"|[A-Za-z_]\w*"                           # identifier / keyword
    r"|" + "|".join(re.escape(p) for p in punctuators) +
    r"|\S")                                    # anything else -> error

escapes = {"n": 10, "t": 9, "r": 13, "0": 0, "\\": 92, "'": 39,
           '"': 34, "a": 7, "b": 8, "f": 12, "v": 11}


def floatToBinary32(f):
    """Convert a Python float to IEEE-754 binary32 bit pattern (as 32-bit int)."""
    import struct
    bits = struct.unpack('<I', struct.pack('<f', f))[0]
    return bits & 0xffffffff


def decodeEscapes(text, line):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            e = text[i + 1]
            if e in escapes:
                out.append(chr(escapes[e]))
                i += 2
            elif e == "x":
                match = re.match(r"[0-9a-fA-F]+", text[i + 2:])
                if not match:
                    fail("invalid hex escape", line)
                out.append(chr(int(match.group(0), 16) & 0xff))
                i += 2 + len(match.group(0))
            elif e.isdigit():
                match = re.match(r"[0-7]{1,3}", text[i + 1:])
                out.append(chr(int(match.group(0), 8) & 0xff))
                i += 1 + len(match.group(0))
            else:
                fail("unknown escape sequence \\" + e, line)
        else:
            out.append(c)
            i += 1
    return "".join(out)


class Token:
    def __init__(self, type, name, line, value=None):
        self.type = type       # keyword/punctuator text, or IDENTIFIER/CONSTANT/STRING
        self.name = name       # original text
        self.line = line
        self.value = value     # int for CONSTANT, decoded str for STRING

    def __repr__(self):
        return "%s(%r)" % (self.type, self.name)


def lexLine(text, line):
    tokens = []
    pos = 0
    for match in tokenPattern.finditer(text):
        t = match.group(0)
        if t[0] == '"':
            tokens.append(Token("STRING", t, line, decodeEscapes(t[1:-1], line)))
        elif t[0] == "'":
            decoded = decodeEscapes(t[1:-1], line)
            if len(decoded) != 1:
                fail("invalid character constant " + t, line)
            tokens.append(Token("CONSTANT", t, line, ord(decoded)))
        elif t in punctuators or t == "#":
            tokens.append(Token(t, t, line))
        elif t[0].isdigit() or t[0] == ".":
            # Float literal: contains digit(s), a dot, or e/E exponent (decimal only),
            # or f/F suffix (not on hex numbers)
            hasDigit = any(c.isdigit() for c in t)
            isHex = t.startswith("0x") or t.startswith("0X")
            isFloat = hasDigit and not isHex and (
                any(c in t for c in ".eE") or
                t.endswith("f") or t.endswith("F"))
            if isFloat:
                if t.startswith("0x") or t.startswith("0X"):
                    fail("hex floats not yet supported", line)
                # Parse decimal float
                s = t.rstrip("fF")
                fval = float(s)
                ival = floatToBinary32(fval)
                tokens.append(Token("FLOAT", t, line, ival))
            else:
                number = t.rstrip("uUlL")
                if number.lower().startswith("0x"):
                    value = int(number, 16)
                elif number.startswith("0") and len(number) > 1:
                    value = int(number, 8)
                else:
                    value = int(number)
                tokens.append(Token("CONSTANT", t, line, value))
        elif t[0].isalpha() or t[0] == "_":
            tokens.append(Token(t if t in keywords else "IDENTIFIER", t, line))
        else:
            fail("unknown token: " + t, line)
    return tokens


############################### PREPROCESSOR ###############################

class Macro:
    def __init__(self, name, params, body):
        self.name = name
        self.params = params   # None for object-like macros
        self.body = body       # list of tokens


class Preprocessor:
    def __init__(self, includeDirs):
        self.macros = {"__RISKY__": Macro("__RISKY__", None,
                                          [Token("CONSTANT", "1", 0, 1)])}
        self.includeDirs = includeDirs
        self.output = []
        self.depth = 0

    def stripComments(self, text):
        out = []
        i = 0
        state = None            # None, '"', "'", "block", "line"
        while i < len(text):
            c = text[i]
            two = text[i:i + 2]
            if state is None:
                if two == "/*":
                    state = "block"
                    i += 2
                    continue
                if two == "//":
                    state = "line"
                    i += 2
                    continue
                if c == '"' or c == "'":
                    state = c
                out.append(c)
            elif state == "block":
                if c == "\n":
                    out.append(c)   # keep line numbers stable
                if two == "*/":
                    state = None
                    i += 2
                    continue
            elif state == "line":
                if c == "\n":
                    out.append(c)
                    state = None
            else:                   # inside a literal
                if two == "\\" + state or two == "\\\\":
                    out.append(two)
                    i += 2
                    continue
                if c == state or c == "\n":
                    state = None
                out.append(c)
            i += 1
        return "".join(out)

    def process(self, fileName):
        global currentSourceName
        if self.depth > 32:
            fail("#include nested too deeply (recursive include?)")
        self.depth += 1
        oldSource = currentSourceName
        currentSourceName = fileName

        try:
            with open(fileName) as f:
                text = f.read()
        except IOError:
            currentSourceName = oldSource
            fail("cannot open " + fileName)

        text = self.stripComments(text)
        # join continuation lines, keeping line numbers via filler
        lines = []
        pending = ""
        pendingStart = None
        for number, line in enumerate(text.split("\n"), 1):
            if line.endswith("\\"):
                pending += line[:-1] + " "
                if pendingStart is None:
                    pendingStart = number
                continue
            lines.append((pendingStart or number, pending + line))
            pending = ""
            pendingStart = None
        if pending:
            lines.append((pendingStart, pending))

        condStack = []          # (active, seenElse, everTrue)
        for number, line in lines:
            stripped = line.strip()
            active = all(entry[0] for entry in condStack)
            if stripped.startswith("#"):
                self.directive(stripped[1:].strip(), number, condStack,
                               active, fileName)
            elif active and stripped:
                tokens = lexLine(line, number)
                # Replace __FILE__ and __LINE__ predefined macros
                for j, tok in enumerate(tokens):
                    if tok.type == "IDENTIFIER":
                        if tok.name == "__LINE__":
                            tokens[j] = Token("CONSTANT", str(number),
                                              number, number)
                        elif tok.name == "__FILE__":
                            tokens[j] = Token("STRING", None,
                                              number, 0)
                            tokens[j].value = fileName
                self.output.extend(self.expand(tokens))

        if condStack:
            fail("unterminated #if/#ifdef", lines[-1][0] if lines else None)
        currentSourceName = oldSource
        self.depth -= 1

    def directive(self, text, line, condStack, active, fileName):
        parts = text.split(None, 1)
        if not parts:
            return
        name = parts[0]
        rest = parts[1] if len(parts) > 1 else ""

        if name == "ifdef" or name == "ifndef":
            value = rest.split()[0] in self.macros if rest.split() else False
            if name == "ifndef":
                value = not value
            condStack.append([active and value, False, value])
        elif name == "if":
            value = bool(self.evalCondition(rest, line)) if active else False
            condStack.append([active and value, False, value])
        elif name == "elif":
            if not condStack or condStack[-1][1]:
                fail("#elif without #if", line)
            entry = condStack[-1]
            outerActive = all(e[0] for e in condStack[:-1])
            if entry[2]:
                entry[0] = False
            else:
                value = bool(self.evalCondition(rest, line)) if outerActive else False
                entry[0] = outerActive and value
                entry[2] = entry[2] or value
        elif name == "else":
            if not condStack or condStack[-1][1]:
                fail("#else without #if", line)
            entry = condStack[-1]
            outerActive = all(e[0] for e in condStack[:-1])
            entry[0] = outerActive and not entry[2]
            entry[1] = True
        elif name == "endif":
            if not condStack:
                fail("#endif without #if", line)
            condStack.pop()
        elif not active:
            return
        elif name == "include":
            match = re.match(r'"([^"]*)"|<([^>]*)>', rest)
            if not match:
                fail("malformed #include", line)
            target = match.group(1) or match.group(2)
            searchDirs = list(self.includeDirs)
            if match.group(1):  # quoted: search the including file's dir first
                searchDirs.insert(0, os.path.dirname(os.path.abspath(fileName)))
            for directory in searchDirs:
                candidate = os.path.join(directory, target)
                if os.path.isfile(candidate):
                    self.process(candidate)
                    return
            fail("include file not found: " + target, line)
        elif name == "define":
            match = re.match(r"([A-Za-z_]\w*)(\()?", rest)
            if not match:
                fail("malformed #define", line)
            macroName = match.group(1)
            if match.group(2):  # function-like
                closing = rest.index(")")
                params = [p.strip() for p in rest[match.end(1) + 1:closing].split(",")]
                params = [p for p in params if p]
                body = lexLine(rest[closing + 1:], line)
                self.macros[macroName] = Macro(macroName, params, body)
            else:
                body = lexLine(rest[match.end(1):], line)
                self.macros[macroName] = Macro(macroName, None, body)
        elif name == "undef":
            self.macros.pop(rest.split()[0] if rest.split() else "", None)
        elif name == "error":
            fail("#error " + rest, line)
        elif name == "pragma":
            pass
        else:
            fail("unknown preprocessor directive #" + name, line)

    def evalCondition(self, text, line):
        # replace defined(X) / defined X before macro expansion
        def replaceDefined(match):
            name = match.group(1) or match.group(2)
            return "1" if name in self.macros else "0"
        text = re.sub(r"defined\s*\(\s*([A-Za-z_]\w*)\s*\)|defined\s+([A-Za-z_]\w*)",
                      replaceDefined, text)
        tokens = self.expand(lexLine(text, line))
        # remaining identifiers evaluate to 0 (C89 semantics)
        tokens = [Token("CONSTANT", "0", line, 0) if t.type == "IDENTIFIER" else t
                  for t in tokens]
        tokens.append(None)
        expression = Parser(tokens).parseConditionalExpression()
        return evalConst(expression, "in #if condition")

    def expand(self, tokens, hidden=frozenset()):
        out = []
        i = 0
        while i < len(tokens):
            token = tokens[i]
            if token.type == "IDENTIFIER" and token.name in self.macros \
                    and token.name not in hidden:
                macro = self.macros[token.name]
                if macro.params is None:
                    out.extend(self.expand(macro.body, hidden | {macro.name}))
                    i += 1
                    continue
                if i + 1 < len(tokens) and tokens[i + 1].type == "(":
                    arguments, i = self.collectArguments(tokens, i + 1, token.line)
                    if len(arguments) != len(macro.params) and \
                            not (len(macro.params) == 0 and arguments == [[]]):
                        fail("macro %s expects %d arguments, got %d"
                             % (macro.name, len(macro.params), len(arguments)),
                             token.line)
                    substituted = []
                    for bodyToken in macro.body:
                        if bodyToken.type == "IDENTIFIER" and bodyToken.name in macro.params:
                            substituted.extend(arguments[macro.params.index(bodyToken.name)])
                        else:
                            substituted.append(bodyToken)
                    out.extend(self.expand(substituted, hidden | {macro.name}))
                    continue
            out.append(token)
            i += 1
        return out

    def collectArguments(self, tokens, i, line):
        # tokens[i] is '('; returns (argument token lists, index after ')')
        arguments = [[]]
        depth = 0
        while i < len(tokens):
            token = tokens[i]
            if token.type == "(":
                depth += 1
                if depth > 1:
                    arguments[-1].append(token)
            elif token.type == ")":
                depth -= 1
                if depth == 0:
                    return arguments, i + 1
                arguments[-1].append(token)
            elif token.type == "," and depth == 1:
                arguments.append([])
            else:
                arguments[-1].append(token)
            i += 1
        fail("unterminated macro argument list", line)


############################### TYPES ###############################

class CType:
    kind = "?"
    size = 0
    isConst = False
    isVolatile = False

    def isInteger(self):
        return self.kind == "int"

    def isPointer(self):
        return self.kind == "pointer"

    def isArray(self):
        return self.kind == "array"

    def isStruct(self):
        return self.kind == "struct"

    def isFunction(self):
        return self.kind == "function"

    def isScalar(self):
        return self.kind in ("int", "long", "float", "pointer")


class IntType(CType):
    kind = "int"
    size = 1

    def __init__(self, name="int", isUnsigned=False):
        self.name = name
        self.isUnsigned = isUnsigned

    def __str__(self):
        return self.name


class LongType(CType):
    kind = "long"
    size = 2

    def __init__(self, isUnsigned=False):
        self.isUnsigned = isUnsigned

    def __str__(self):
        return ("unsigned " if self.isUnsigned else "") + "long"


class FloatType(CType):
    kind = "float"
    size = 2

    def __str__(self):
        return "float"


class VoidType(CType):
    kind = "void"
    size = 0

    def __str__(self):
        return "void"


class PointerType(CType):
    kind = "pointer"
    size = 1

    def __init__(self, target):
        self.target = target

    def __str__(self):
        return str(self.target) + " *"


class ArrayType(CType):
    kind = "array"

    def __init__(self, element, length):
        self.element = element
        self.length = length        # may be None (incomplete)
        self.size = (length or 0) * element.size

    def __str__(self):
        return "%s[%s]" % (self.element, self.length if self.length else "")


class StructType(CType):
    kind = "struct"

    def __init__(self, tag, isUnion):
        self.tag = tag
        self.isUnion = isUnion
        self.members = None         # list of (name, type, offset)
        self.size = 0

    def define(self, memberList):
        self.members = []
        offset = 0
        for name, memberType in memberList:
            if memberType.size == 0 and not memberType.isStruct():
                fail("struct member '%s' has incomplete type" % name)
            self.members.append((name, memberType, 0 if self.isUnion else offset))
            if self.isUnion:
                self.size = max(self.size, memberType.size)
            else:
                offset += memberType.size
        if not self.isUnion:
            self.size = offset

    def member(self, name, line=None):
        if self.members is None:
            fail("use of incomplete type '%s'" % self, line)
        for memberName, memberType, offset in self.members:
            if memberName == name:
                return memberType, offset
        fail("'%s' has no member named '%s'" % (self, name), line)

    def __str__(self):
        return ("union " if self.isUnion else "struct ") + (self.tag or "<anonymous>")


class FunctionType(CType):
    kind = "function"
    size = 1

    def __init__(self, returnType, params, ellipsis):
        self.returnType = returnType
        self.params = params        # list of (name, type) or None (unknown)
        self.ellipsis = ellipsis

    def __str__(self):
        return str(self.returnType) + " (*)()"


INT = IntType("int")
CHAR = IntType("char")
LONG = LongType()
FLOAT = FloatType()
VOID = VoidType()
CHARPTR = PointerType(CHAR)


def decay(ctype):
    """array-of-T decays to pointer-to-T, function to pointer-to-function"""
    if ctype.isArray():
        return PointerType(ctype.element)
    if ctype.isFunction():
        return PointerType(ctype)
    return ctype


############################### CONSTANT EXPRESSIONS ###############################

class NotConstant(Exception):
    pass


def evalConst(node, context="constant expression"):
    try:
        return evalConstInner(node) & 0xffff
    except NotConstant:
        fail("expression is not constant (%s)" % context,
             getattr(node, "line", None))


def evalConstSigned(node, context="constant expression"):
    value = evalConst(node, context)
    return value - 0x10000 if value & 0x8000 else value


def containsWideConstant(node):
    """True if a constant expression involves values beyond 16 bits
    (wide constants or casts to a 32-bit type), so folding it with
    16-bit int semantics would corrupt it."""
    if isinstance(node, Constant):
        return node.value > 0xffff
    if isinstance(node, FloatConstant):
        return True
    if isinstance(node, UnaryOperation):
        return containsWideConstant(node.operand)
    if isinstance(node, BinaryOperator):
        return containsWideConstant(node.expressionLeft) or \
               containsWideConstant(node.expressionRight)
    if isinstance(node, Typecast):
        return is32Bit(node.typeName) or \
               containsWideConstant(node.castExpression)
    if isinstance(node, ConditionalExpression):
        return containsWideConstant(node.conditionExpression) or \
               containsWideConstant(node.trueExpression) or \
               containsWideConstant(node.falseExpression)
    if isinstance(node, Expression):
        for e in node.expressionList:
            if containsWideConstant(e):
                return True
    return False


def evalConstInner(node):
    if isinstance(node, Constant):
        return node.value
    if isinstance(node, UnaryOperation):
        value = evalConstInner(node.operand)
        signedValue = value - 0x10000 if value & 0x8000 else value
        if node.operator == "-":
            return -signedValue
        if node.operator == "+":
            return signedValue
        if node.operator == "~":
            return ~value
        if node.operator == "!":
            return int(value == 0)
        raise NotConstant()
    if isinstance(node, BinaryOperator):
        left = evalConstInner(node.expressionLeft) & 0xffff
        right = evalConstInner(node.expressionRight) & 0xffff
        sleft = left - 0x10000 if left & 0x8000 else left
        sright = right - 0x10000 if right & 0x8000 else right
        op = node.operator
        if op == "+": return sleft + sright
        if op == "-": return sleft - sright
        if op == "*": return sleft * sright
        if op == "/":
            if sright == 0: raise NotConstant()
            return int(sleft / sright)
        if op == "%":
            if sright == 0: raise NotConstant()
            return sleft - int(sleft / sright) * sright
        if op == "<<": return left << (right & 0x1f)
        if op == ">>": return left >> (right & 0x1f)
        if op == "&": return left & right
        if op == "|": return left | right
        if op == "^": return left ^ right
        if op == "==": return int(sleft == sright)
        if op == "!=": return int(sleft != sright)
        if op == "<": return int(sleft < sright)
        if op == ">": return int(sleft > sright)
        if op == "<=": return int(sleft <= sright)
        if op == ">=": return int(sleft >= sright)
        if op == "&&": return int(bool(sleft) and bool(sright))
        if op == "||": return int(bool(sleft) or bool(sright))
        raise NotConstant()
    if isinstance(node, ConditionalExpression):
        return evalConstInner(node.trueExpression) if evalConstInner(node.conditionExpression) \
            else evalConstInner(node.falseExpression)
    if isinstance(node, Typecast):
        return evalConstInner(node.castExpression)
    if isinstance(node, SizeofType):
        return node.typeName.size
    if isinstance(node, VariableAccess):
        if node.name in enumConstants:
            return enumConstants[node.name]
        raise NotConstant()
    if isinstance(node, Expression) and len(node.expressionList) == 1:
        return evalConstInner(node.expressionList[0])
    raise NotConstant()


############################### CODE GENERATOR STATE ###############################

class Symbol:
    def __init__(self, name, ctype, kind, offset=None, address=None):
        self.name = name
        self.ctype = ctype
        self.kind = kind            # 'local', 'param', 'global', 'function'
        self.offset = offset        # r15-relative for local/param
        self.address = address      # RAM address for globals
        self.defined = False        # for functions
        self.hasInit = False        # for globals


class Scope:
    def __init__(self, parent=None):
        self.parent = parent
        self.symbols = {}

    def declare(self, symbol, line=None):
        if symbol.name in self.symbols:
            fail("redeclaration of '%s'" % symbol.name, line)
        self.symbols[symbol.name] = symbol

    def lookup(self, name):
        scope = self
        while scope is not None:
            if name in scope.symbols:
                return scope.symbols[name]
            scope = scope.parent
        return None


class FunctionContext:
    def __init__(self, name, returnType):
        self.name = name
        self.returnType = returnType
        self.cur = 0                # next free local slot (r15-relative)
        self.minCur = 0
        self.breakLabels = []
        self.continueLabels = []
        self.usedLabels = set()

    def allocLocal(self, size):
        base = self.cur - size + 1
        self.cur -= size
        self.minCur = min(self.minCur, self.cur)
        return base

    def frameSize(self):
        return -self.minCur


class Codegen:
    def __init__(self):
        self.globalScope = Scope()
        self.functions = {}         # name -> Symbol
        self.globalCursor = 0
        self.globalInits = []       # (address, valueSpec) valueSpec: int | ('str', i) | ('addr', name)
        self.strings = []           # list of decoded strings
        self.stringAddresses = []
        self.currentFunction = None # FunctionContext
        self.neededHelpers = set()  # {"__sdiv", "__smod"} — emitted at link time

    def runtimeHelpers(self):
        if not self.neededHelpers:
            return ""
        code = ""
        if "__sdiv" in self.neededHelpers:
            la = createUniqueLabel(); la2 = createUniqueLabel()
            lb = createUniqueLabel(); lc = createUniqueLabel()
            code += ("\n__sdiv:"
                     "\n\tin r3,#0"
                     "\n\tmov r2,r1"
                     "\n\tandi r2,#32768"
                     "\n\tcmp r2,#0"
                     "\n\tjeq " + la +
                     "\n\tneg r1,r1"
                     "\n\tin r3,#1"
                     "\n\tjmp " + la2 +
                     "\n" + la + ":"
                     "\n\tin r3,#0"
                     "\n" + la2 + ":"
                     "\n\tmov r2,r0"
                     "\n\tandi r2,#32768"
                     "\n\tcmp r2,#0"
                     "\n\tjeq " + lb +
                     "\n\tneg r0,r0"
                     "\n\txori r3,#1"
                     "\n" + lb + ":"
                     "\n\tdiv r0,r1,r0"
                     "\n\tcmp r3,#0"
                     "\n\tjeq " + lc +
                     "\n\tneg r0,r0"
                     "\n" + lc + ":"
                     "\n\tret")
        if "__smod" in self.neededHelpers:
            ld = createUniqueLabel(); ld2 = createUniqueLabel()
            le = createUniqueLabel(); lf = createUniqueLabel()
            code += ("\n__smod:"
                     "\n\tin r3,#0"
                     "\n\tmov r2,r1"
                     "\n\tandi r2,#32768"
                     "\n\tcmp r2,#0"
                     "\n\tjeq " + ld +
                     "\n\tneg r1,r1"
                     "\n\tin r3,#1"
                     "\n\tjmp " + ld2 +
                     "\n" + ld + ":"
                     "\n\tin r3,#0"
                     "\n" + ld2 + ":"
                     "\n\tmov r2,r0"
                     "\n\tandi r2,#32768"
                     "\n\tcmp r2,#0"
                     "\n\tjeq " + le +
                     "\n\tneg r0,r0"
                     "\n" + le + ":"
                     "\n\tmod r0,r1,r0"
                     "\n\tcmp r3,#0"
                     "\n\tjeq " + lf +
                     "\n\tneg r0,r0"
                     "\n" + lf + ":"
                     "\n\tret")
        if "__sdiv32" in self.neededHelpers:
            l32a = createUniqueLabel(); l32a2 = createUniqueLabel()
            l32b = createUniqueLabel(); l32c = createUniqueLabel()
            code += ("\n__sdiv32:"            # er0=dividend, er1=divisor
                     # Stack has: [..., div_lo, div_hi, ret_addr]
                     # Pop ret_addr, then divisor
                     "\n\tpop r5"             # ret addr → r5
                     "\n\tpop r2"             # divisor hi → er1
                     "\n\tpop r3"             # divisor lo → er1
                     "\n\tpush r5"            # ret addr back on stack
                     "\n\tin r4,#0"           # r4 = sign flag
                     "\n\tmov r5,r0"          # check dividend sign (hi word)
                     "\n\tandi r5,#32768"
                     "\n\tcmp r5,#0"
                     "\n\tjeq " + l32a +
                     "\n\tneg32 er0,er0"      # make dividend positive
                     "\n\tin r4,#1"
                     "\n\tjmp " + l32a2 +
                     "\n" + l32a + ":"
                     "\n\tin r4,#0"
                     "\n" + l32a2 + ":"
                     "\n\tmov r5,r2"          # check divisor sign
                     "\n\tandi r5,#32768"
                     "\n\tcmp r5,#0"
                     "\n\tjeq " + l32b +
                     "\n\tneg32 er1,er1"      # make divisor positive
                     "\n\txori r4,#1"         # toggle sign flag
                     "\n" + l32b + ":"
                     "\n\tdiv32 er0,er1"      # unsigned division
                     "\n\tcmp r4,#0"
                     "\n\tjeq " + l32c +
                     "\n\tneg32 er0,er0"      # apply sign
                     "\n" + l32c + ":"
                     "\n\tpop r5"             # ret addr
                     "\n\tjmpr r5")
        if "__smod32" in self.neededHelpers:
            l32d = createUniqueLabel(); l32d2 = createUniqueLabel()
            l32e = createUniqueLabel(); l32f = createUniqueLabel()
            code += ("\n__smod32:"            # er0=dividend, er1=divisor
                     "\n\tpop r5"             # ret addr → r5
                     "\n\tpop r2"             # divisor hi → er1
                     "\n\tpop r3"             # divisor lo → er1
                     "\n\tpush r5"            # ret addr back on stack
                     "\n\tin r4,#0"           # r4 = sign flag
                     "\n\tmov r5,r0"          # check dividend sign
                     "\n\tandi r5,#32768"
                     "\n\tcmp r5,#0"
                     "\n\tjeq " + l32d +
                     "\n\tneg32 er0,er0"      # make dividend positive
                     "\n\tin r4,#1"
                     "\n\tjmp " + l32d2 +
                     "\n" + l32d + ":"
                     "\n\tin r4,#0"
                     "\n" + l32d2 + ":"
                     "\n\tmov r5,r2"          # check divisor sign
                     "\n\tandi r5,#32768"
                     "\n\tcmp r5,#0"
                     "\n\tjeq " + l32e +
                     "\n\tneg32 er1,er1"      # make divisor positive
                     "\n" + l32e + ":"
                     "\n\tmod32 er0,er1"      # unsigned mod
                     "\n\tcmp r4,#0"
                     "\n\tjeq " + l32f +
                     "\n\tneg32 er0,er0"      # apply sign
                     "\n" + l32f + ":"
                     "\n\tpop r5"             # ret addr
                     "\n\tjmpr r5")
        return code

    def internString(self, text):
        for i, existing in enumerate(self.strings):
            if existing == text:
                return i
        self.strings.append(text)
        return len(self.strings) - 1

    def allocGlobal(self, size):
        address = self.globalCursor
        self.globalCursor += size
        if self.globalCursor > 0x8000:
            fail("global variables exceed available data memory (0x8000 words)")
        return address


cg = None                           # set in main()
enumConstants = {}                  # name -> int
structTags = {}                     # tag -> StructType
typedefNames = {}                   # name -> CType

HEAP_BASE = 0x8000


def createUniqueLabel():
    createUniqueLabel.counter += 1
    return "__L%d" % createUniqueLabel.counter
createUniqueLabel.counter = 0


def emitLongConstant(value):
    """Load a 32-bit value into r0 (hi) : r1 (lo)."""
    hi = (value >> 16) & 0xffff
    lo = value & 0xffff
    return "\n\tin r0,#" + str(hi) + "\n\tin r1,#" + str(lo)


def loadFromAddress(ctype):
    """value of an lvalue whose address is in r0"""
    if is32Bit(ctype):
        # word layout: [addr] = hi, [addr+1] = lo (matches VariableAccess
        # and Assignment)
        return "\n\tmov r2,r0\n\tldr r0,*r2\n\tldo r1,*r2,#1"
    if ctype.isScalar():
        return "\n\tldr r0,*r0"
    return ""                       # arrays/structs: address is the value


def isLong(ctype):
    """Check if a type is a 32-bit long."""
    return hasattr(ctype, 'kind') and ctype.kind == "long"


def isFloat(ctype):
    """Check if a type is a 32-bit float."""
    return hasattr(ctype, 'kind') and ctype.kind == "float"


def is32Bit(ctype):
    """Check if a type is 32-bit (long or float)."""
    return isLong(ctype) or isFloat(ctype)


def emitConversion(code, srcType, dstType):
    """Append code converting the value in r0 (16-bit) / r0:r1 (32-bit)
    from srcType to dstType.

    int <-> float converts numerically by calling the float32.h runtime
    helpers (the program must #include <float32.h>).  long <-> float
    reinterprets the bits: 'long' is the float32 library's carrier type
    (f32_make, f32_add, ... all traffic in longs), so implicit numeric
    conversion here would break every direct f32_* call.  Use
    f32_from_long/f32_to_long for numeric long <-> float conversion."""
    if srcType is None or dstType is None:
        return code
    srcArith = srcType.isScalar() and not srcType.isPointer()
    dstArith = dstType.isScalar() and not dstType.isPointer()
    if isFloat(dstType) and not is32Bit(srcType) and srcArith:
        if getattr(srcType, "isUnsigned", False):
            # zero-extend and convert as long so 0x8000..0xffff stay positive
            code += "\n\tmov r1,r0\n\tin r0,#0" \
                  + "\n\tpush r1\n\tpush r0\n\tcall f32_from_long\n\tpop\n\tpop"
        else:
            code += "\n\tpush r0\n\tcall f32_from_int\n\tpop"
    elif isFloat(srcType) and not is32Bit(dstType) and dstArith:
        code += "\n\tpush r1\n\tpush r0\n\tcall f32_to_int\n\tpop\n\tpop"
    elif is32Bit(dstType) and not is32Bit(srcType):
        code += "\n\tmov r1,r0\n\tin r0,#0"
    elif not is32Bit(dstType) and is32Bit(srcType):
        code += "\n\tmov r0,r1"
    return code


def genTruthTest(node):
    """cmp-against-zero for a just-generated expression value,
    handling 32-bit operands (long: hi|lo, float: also ignore the
    sign bit so -0.0 is false)."""
    t = getattr(node, "ctype", None)
    if isFloat(t):
        return "\n\tandi r0,#32767\n\tor r0,r0,r1\n\tcmp r0,#0"
    if isLong(t):
        return "\n\tor r0,r0,r1\n\tcmp r0,#0"
    return "\n\tcmp r0,#0"


def genScaledIndex(indexCode, scale):
    code = indexCode
    if scale != 1:
        code += "\n\tlmuli r0,#" + str(scale)
    return code


def simpleVarSymbol(node, scope):
    """Symbol for a plain local/param/global scalar variable reference,
    or None if the expression is anything more complex.  Such variables
    can be loaded/stored with a single ldo/ldd/stoo/stod instead of
    materializing the address."""
    if not isinstance(node, VariableAccess):
        return None
    symbol = scope.lookup(node.name)
    if symbol is None:
        return None
    if symbol.kind not in ("local", "param", "global"):
        return None
    if not symbol.ctype.isScalar():
        return None
    return symbol


def loadSimpleVar(symbol, reg):
    """load a 16-bit simple variable into the given register"""
    if symbol.kind == "global":
        return "\n\tldd %s,*%d" % (reg, symbol.address)
    return "\n\tldo %s,*r15,#%d" % (reg, symbol.offset & 0xffff)


def storeSimpleVar(symbol, reg="r0"):
    """store reg (r0:r1 for 32-bit types) into a simple variable"""
    if is32Bit(symbol.ctype):
        if symbol.kind == "global":
            return "\n\tstod *%d,r0\n\tstod *%d,r1" \
                   % (symbol.address, symbol.address + 1)
        return "\n\tstoo *r15,#%d,r0\n\tstoo *r15,#%d,r1" \
               % (symbol.offset & 0xffff, (symbol.offset + 1) & 0xffff)
    if symbol.kind == "global":
        return "\n\tstod *%d,%s" % (symbol.address, reg)
    return "\n\tstoo *r15,#%d,%s" % (symbol.offset & 0xffff, reg)


############################### AST: EXPRESSIONS ###############################

class Node:
    line = None

    def generate(self, scope):
        raise NotImplementedError(type(self).__name__)

    def generateAddress(self, scope):
        fail("expression is not an lvalue", self.line)


class Constant(Node):
    def __init__(self, value):
        self.value = value
        self.ctype = INT

    def generate(self, scope):
        self.ctype = INT
        return "\n\tin r0,#" + str(self.value & 0xffff)


class FloatConstant(Node):
    """IEEE-754 binary32 float literal. Stores the 32-bit bit pattern."""
    def __init__(self, bits):
        self.bits = bits & 0xffffffff    # 32-bit IEEE-754 bit pattern
        self.ctype = FLOAT

    def generate(self, scope):
        self.ctype = FLOAT
        hi = (self.bits >> 16) & 0xffff
        lo = self.bits & 0xffff
        return "\n\tin r0,#" + str(hi) + "\n\tin r1,#" + str(lo)


class StringLiteral(Node):
    def __init__(self, text):
        self.text = text

    def generate(self, scope):
        self.ctype = PointerType(CHAR)
        index = cg.internString(self.text)
        return "\n\tin r0,#@STR%d@" % index


class VariableAccess(Node):
    def __init__(self, name):
        self.name = name

    def resolve(self, scope):
        symbol = scope.lookup(self.name)
        if symbol is None and self.name in enumConstants:
            return "enum"
        if symbol is None and self.name in cg.functions:
            return cg.functions[self.name]
        if symbol is None:
            fail("'%s' undeclared" % self.name, self.line)
        return symbol

    def generate(self, scope):
        symbol = self.resolve(scope)
        if symbol == "enum":
            self.ctype = INT
            return "\n\tin r0,#" + str(enumConstants[self.name] & 0xffff)
        if symbol.kind == "function":
            self.ctype = PointerType(symbol.ctype)
            return "\n\tin r0,#" + symbol.name
        self.ctype = symbol.ctype
        if is32Bit(symbol.ctype):
            if symbol.kind == "global":
                return "\n\tldd r0,*" + str(symbol.address) \
                     + "\n\tldd r1,*" + str(symbol.address + 1)
            return "\n\tldo r0,*r15,#" + str(symbol.offset & 0xffff) \
                 + "\n\tldo r1,*r15,#" + str((symbol.offset + 1) & 0xffff)
        if symbol.ctype.isScalar():
            if symbol.kind == "global":
                return "\n\tldd r0,*" + str(symbol.address)
            return "\n\tldo r0,*r15,#" + str(symbol.offset)
        # arrays and structs: the value is the address
        return self.generateAddress(scope)

    def generateAddress(self, scope):
        symbol = self.resolve(scope)
        if symbol == "enum":
            fail("'%s' is not an lvalue" % self.name, self.line)
        if symbol.kind == "function":       # &func yields the function address
            self.ctype = symbol.ctype
            return "\n\tin r0,#" + symbol.name
        self.ctype = symbol.ctype
        if symbol.kind == "global":
            return "\n\tin r0,#" + str(symbol.address)
        code = "\n\tmov r0,r15"
        if symbol.offset != 0:
            code += "\n\taddi r0,#" + str(symbol.offset & 0xffff)
        return code


class ArrayAccess(Node):
    def __init__(self, base, index):
        self.base = base
        self.index = index

    def generateAddress(self, scope):
        code = self.base.generate(scope)
        baseType = self.base.ctype
        indexNode = self.index
        if baseType.isInteger():        # support  i[a]
            self.base, self.index = self.index, self.base
            code = self.base.generate(scope)
            baseType = self.base.ctype
            indexNode = self.index
        if baseType.isArray():
            element = baseType.element
        elif baseType.isPointer():
            element = baseType.target
        else:
            fail("subscripted value is not an array or pointer", self.line)
        self.ctype = element
        code += "\n\tpush r0" \
              + genScaledIndex(indexNode.generate(scope), max(element.size, 1)) \
              + "\n\tpop r1" \
              + "\n\tadd r0,r1,r0"
        return code

    def generate(self, scope):
        code = self.generateAddress(scope)
        return code + loadFromAddress(self.ctype)


class MemberAccess(Node):
    def __init__(self, base, memberName, arrow):
        self.base = base
        self.memberName = memberName
        self.arrow = arrow

    def generateAddress(self, scope):
        # struct-valued expressions (variables, s.a.b, p->m, a[i]) all leave
        # the struct's ADDRESS in r0 when generated, so '.' uses generate()
        code = self.base.generate(scope)
        baseType = self.base.ctype
        if self.arrow:
            if not (baseType.isPointer() and baseType.target.isStruct()):
                fail("'->' applied to non-pointer-to-struct", self.line)
            structType = baseType.target
        else:
            if not baseType.isStruct():
                fail("'.' applied to non-struct", self.line)
            structType = baseType
        memberType, offset = structType.member(self.memberName, self.line)
        self.ctype = memberType
        if offset != 0:
            code += "\n\taddi r0,#" + str(offset)
        return code

    def generate(self, scope):
        code = self.generateAddress(scope)
        return code + loadFromAddress(self.ctype)


class FunctionCall(Node):
    def __init__(self, callee, arguments):
        self.callee = callee
        self.arguments = arguments

    def generate(self, scope):
        # figure out what we're calling
        directName = None
        functionType = None
        if isinstance(self.callee, VariableAccess):
            name = self.callee.name
            if scope.lookup(name) is None:
                if name not in cg.functions:
                    warn("implicit declaration of function '%s'" % name, self.line)
                    symbol = Symbol(name, FunctionType(INT, None, False), "function")
                    cg.functions[name] = symbol
                directName = name
                functionType = cg.functions[name].ctype
        if directName is None:
            calleeCode = self.callee.generate(scope)
            calleeType = self.callee.ctype
            if calleeType.isPointer() and calleeType.target.isFunction():
                functionType = calleeType.target
            elif calleeType.isFunction():
                functionType = calleeType
            else:
                fail("called object is not a function", self.line)

        if functionType.params is not None:
            expected = len(functionType.params)
            got = len(self.arguments)
            if (functionType.ellipsis and got < expected) or \
                    (not functionType.ellipsis and got != expected):
                fail("wrong number of arguments in call (expected %d, got %d)"
                     % (expected, got), self.line)

        code = ""
        totalWords = 0
        paramTypes = functionType.params if functionType.params is not None else []
        for idx, argument in enumerate(reversed(self.arguments)):
            # Use expected parameter type if available (for int→long promotion)
            expectedType = None
            if idx < len(paramTypes):
                expectedType = paramTypes[len(paramTypes) - 1 - idx][1]
            # Emit constant directly in 32-bit if expected type is long/float
            longConstVal = None
            if isFloat(expectedType) and isinstance(argument, Constant):
                longConstVal = floatToBinary32(float(argument.value))
            elif isLong(expectedType) and isinstance(argument, Constant):
                longConstVal = argument.value
            elif isLong(expectedType) and not containsWideConstant(argument):
                try:
                    longConstVal = evalConstInner(argument)
                except NotConstant:
                    pass
            if longConstVal is not None:
                code += emitLongConstant(longConstVal)
                argType = FLOAT if isFloat(expectedType) else LONG
            else:
                code += argument.generate(scope)
                argType = argument.ctype
                # convert to the parameter's width/type: int <-> float
                # converts numerically, long <-> int widens/narrows
                # (long <-> float is a bit-level pass-through, see
                # emitConversion)
                if expectedType is not None and \
                        (isFloat(expectedType) != isFloat(argType) or
                         is32Bit(expectedType) != is32Bit(argType)):
                    code = emitConversion(code, argType, expectedType)
                    argType = expectedType
            if argType.isStruct():
                code += "\n\tmov r1,r0"
                for i in reversed(range(argType.size)):
                    code += "\n\tldo r0,*r1,#" + str(i) + "\n\tpush r0"
                totalWords += argType.size
            elif is32Bit(argType) or is32Bit(expectedType):
                # Promote int to long if needed
                if not is32Bit(argType):
                    code += "\n\tmov r1,r0\n\tin r0,#0"
                # Push lo first, then hi → hi at r15+3, lo at r15+4
                code += "\n\tpush r1\n\tpush r0"
                totalWords += 2
            else:
                code += "\n\tpush r0"
                totalWords += 1

        if directName is not None:
            code += "\n\tcall " + directName
        else:
            code += calleeCode + "\n\tcallr r0"

        # drop the arguments; r2 is free (r0/r1 hold the return value)
        if totalWords > 3:
            code += "\n\tldsp r2" \
                  + "\n\taddi r2,#" + str(totalWords) \
                  + "\n\tstosp r2"
        else:
            for _ in range(totalWords):
                code += "\n\tpop"

        self.ctype = functionType.returnType
        if self.ctype.isStruct():
            fail("functions returning structs are not supported", self.line)
        return code


class PostIncDec(Node):
    def __init__(self, operand, operator):
        self.operand = operand
        self.operator = operator    # '++' or '--'

    def generate(self, scope):
        symbol = simpleVarSymbol(self.operand, scope)
        if symbol is not None and not is32Bit(symbol.ctype):
            self.ctype = symbol.ctype
            if self.ctype.isConst:
                warn("modification of const-qualified variable", self.line)
            step = incDecStep(self.ctype, self.line)
            instruction = "addi" if self.operator == "++" else "subi"
            return loadSimpleVar(symbol, "r0") \
                 + "\n\tmov r1,r0" \
                 + "\n\t" + instruction + " r1,#" + str(step) \
                 + storeSimpleVar(symbol, "r1")
        code = self.operand.generateAddress(scope)
        self.ctype = self.operand.ctype
        if self.ctype.isConst:
            warn("modification of const-qualified variable", self.line)
        step = incDecStep(self.ctype, self.line)
        instruction = "addi" if self.operator == "++" else "subi"
        code += "\n\tmov r1,r0" \
              + "\n\tldr r0,*r1" \
              + "\n\tpush r0" \
              + "\n\t" + instruction + " r0,#" + str(step) \
              + "\n\tstor *r1,r0" \
              + "\n\tpop r0"
        return code


class PreIncDec(Node):
    def __init__(self, operand, operator):
        self.operand = operand
        self.operator = operator

    def generate(self, scope):
        symbol = simpleVarSymbol(self.operand, scope)
        if symbol is not None and not is32Bit(symbol.ctype):
            self.ctype = symbol.ctype
            if self.ctype.isConst:
                warn("modification of const-qualified variable", self.line)
            step = incDecStep(self.ctype, self.line)
            instruction = "addi" if self.operator == "++" else "subi"
            return loadSimpleVar(symbol, "r0") \
                 + "\n\t" + instruction + " r0,#" + str(step) \
                 + storeSimpleVar(symbol)
        code = self.operand.generateAddress(scope)
        self.ctype = self.operand.ctype
        if self.ctype.isConst:
            warn("modification of const-qualified variable", self.line)
        step = incDecStep(self.ctype, self.line)
        instruction = "addi" if self.operator == "++" else "subi"
        code += "\n\tmov r1,r0" \
              + "\n\tldr r0,*r1" \
              + "\n\t" + instruction + " r0,#" + str(step) \
              + "\n\tstor *r1,r0"
        return code


def incDecStep(ctype, line):
    if ctype.isPointer():
        return max(ctype.target.size, 1)
    if ctype.isInteger():
        return 1
    fail("++/-- requires an integer or pointer", line)


class UnaryOperation(Node):
    operators = ["&", "*", "+", "-", "~", "!"]

    def __init__(self, operator, operand):
        self.operator = operator
        self.operand = operand

    def generate(self, scope):
        if self.operator == "&":
            code = self.operand.generateAddress(scope)
            self.ctype = PointerType(self.operand.ctype)
            return code
        if self.operator == "*":
            code = self.operand.generate(scope)
            operandType = self.operand.ctype
            if operandType.isPointer():
                self.ctype = operandType.target
            elif operandType.isInteger():
                warn("dereferencing an integer (treated as int *)", self.line)
                self.ctype = INT
            else:
                fail("cannot dereference this type", self.line)
            return code + loadFromAddress(self.ctype)

        if self.operator == "-" and isinstance(self.operand, Constant):
            # fold, keeping wide constants 32-bit (-100000000); values
            # up to 32768 stay 16-bit ints (-32768 is a valid int)
            value = self.operand.value
            if value > 0x8000:
                self.ctype = LONG
                return emitLongConstant(-value & 0xffffffff)
            self.ctype = INT
            return "\n\tin r0,#" + str(-value & 0xffff)

        code = self.operand.generate(scope)
        if not self.operand.ctype.isScalar():
            fail("invalid operand to unary " + self.operator, self.line)
        opLong = isLong(self.operand.ctype)
        opFloat = isFloat(self.operand.ctype)
        if opLong:
            self.ctype = LONG
        elif opFloat:
            self.ctype = FLOAT
        else:
            self.ctype = INT
        if self.operator == "-":
            if opFloat:
                code += "\n\txori r0,#32768"    # flip the sign bit
            elif opLong:
                code += "\n\tneg32 er0,er0"
            else:
                code += "\n\tneg r0,r0"
        elif self.operator == "~":
            if opFloat:
                fail("invalid operand to unary ~ (have float)", self.line)
            if opLong:
                code += "\n\tnot32 er0,er0"
            else:
                code += "\n\tnot r0,r0"
        elif self.operator == "!":
            self.ctype = INT
            if opFloat:
                code += "\n\tandi r0,#32767"    # -0.0 is false too
            if opLong or opFloat:
                code += "\n\tor r2,r0,r1" \
                      + "\n\tcmp r2,#0" \
                      + "\n\tmov r0,state" \
                      + "\n\tandi r0,#16" \
                      + "\n\tshri r0,#4"
            else:
                code += "\n\tcmp r0,#0" \
                      + "\n\tmov r0,state" \
                      + "\n\tandi r0,#16" \
                      + "\n\tshri r0,#4"
        elif self.operator == "+":
            self.ctype = self.operand.ctype
        return code

    def generateAddress(self, scope):
        if self.operator != "*":
            fail("expression is not an lvalue", self.line)
        code = self.operand.generate(scope)
        operandType = self.operand.ctype
        if operandType.isPointer():
            self.ctype = operandType.target
        elif operandType.isInteger():
            warn("dereferencing an integer (treated as int *)", self.line)
            self.ctype = INT
        else:
            fail("cannot dereference this type", self.line)
        return code


class SizeofExpression(Node):
    def __init__(self, operand):
        self.operand = operand

    def generate(self, scope):
        self.operand.generate(scope)        # discard code, we need the type
        self.ctype = INT
        return "\n\tin r0,#" + str(self.operand.ctype.size)


class SizeofType(Node):
    def __init__(self, typeName):
        self.typeName = typeName

    def generate(self, scope):
        self.ctype = INT
        return "\n\tin r0,#" + str(self.typeName.size)


class Typecast(Node):
    def __init__(self, typeName, castExpression):
        self.typeName = typeName
        self.castExpression = castExpression

    def generate(self, scope):
        # (float)intConstant: fold at compile time
        if isFloat(self.typeName) and isinstance(self.castExpression, Constant):
            self.ctype = self.typeName
            bits = floatToBinary32(float(self.castExpression.value))
            return emitLongConstant(bits)
        code = self.castExpression.generate(scope)
        srcType = self.castExpression.ctype
        dstType = self.typeName
        # int <-> float converts numerically; long <-> float passes bits
        # through (long is the float32 library's carrier type); other
        # width changes truncate/zero-extend.
        code = emitConversion(code, srcType, dstType)
        self.ctype = self.typeName
        return code


binaryOpInstructions = {
    "+": "add", "-": "sub", "*": "lmul", "/": "div", "%": "mod",
    "<<": "shl", ">>": "shr", "&": "and", "|": "or", "^": "xor"}

comparisonFlags = {                 # state register bit -> shift
    "==": (16, 4), "!=": (32, 5), "<": (4, 2), ">": (64, 6)}


class BinaryOperator(Node):
    def __init__(self, operator, expressionLeft, expressionRight):
        self.operator = operator
        self.expressionLeft = expressionLeft
        self.expressionRight = expressionRight

    def generate(self, scope):
        op = self.operator

        if op in ("&&", "||"):
            return self.generateLogic(scope)

        # constant folding: evaluate at compile time if possible.
        # Skip operators where signed/unsigned matters or where the result
        # may exceed 16 bits (shifts, division, comparison), and any
        # expression with wide (32-bit) constants or casts.
        if op not in (",", "/", "%", "<", ">", "<=", ">=", ">>", "<<") \
                and not containsWideConstant(self):
            try:
                result = evalConstInner(self) & 0xffff
                self.ctype = INT
                return "\n\tin r0,#" + str(result)
            except NotConstant:
                pass

        # Peek at operand types to detect 32-bit operations
        def peekType(node):
            t = node.ctype if hasattr(node, 'ctype') else None
            # wide constants are emitted as 32-bit values
            if isinstance(node, Constant) and node.value > 0xffff:
                return LONG
            # If the node is a Typecast to a 32-bit type, use that type
            if isinstance(node, Typecast) and is32Bit(node.typeName):
                return node.typeName
            # Look through typecasts for VariableAccess
            inner = node
            while isinstance(inner, Typecast):
                inner = inner.castExpression
            if isinstance(inner, VariableAccess) and (t is None or not is32Bit(t)):
                s = inner.resolve(scope)
                if not isinstance(s, str) and s is not None:
                    return s.ctype
            # Detect chained 32-bit ops: (x >> 16) & 0xFFFF where >> produces long
            if isinstance(node, BinaryOperator) and node.operator in ("<<", ">>"):
                lt = peekType(node.expressionLeft)
                if is32Bit(lt):
                    return LONG
            # Nested arithmetic/bitwise: (x & 0x7FFFFF) | 0x800000
            if isinstance(node, BinaryOperator) and \
                    node.operator in ("+", "-", "*", "/", "%", "&", "|", "^"):
                lt = peekType(node.expressionLeft)
                rt = peekType(node.expressionRight)
                if isFloat(lt) or isFloat(rt):
                    return FLOAT
                if is32Bit(lt) or is32Bit(rt):
                    return LONG
            # Look through unary +/-/~ (e.g.  -x * y  with x long/float)
            if isinstance(node, UnaryOperation) and node.operator in ("+", "-", "~"):
                # negated wide constants fold to 32-bit values
                if node.operator == "-" and isinstance(node.operand, Constant) \
                        and node.operand.value > 0x8000:
                    return LONG
                return peekType(node.operand)
            # Function calls: use the declared return type
            if isinstance(node, FunctionCall) and \
                    isinstance(node.callee, VariableAccess) and \
                    node.callee.name in cg.functions:
                return cg.functions[node.callee.name].ctype.returnType
            # Array element / pointer dereference / struct member
            if isinstance(node, ArrayAccess):
                bt = peekType(node.base)
                if bt is not None and bt.isArray():
                    return bt.element
                if bt is not None and bt.isPointer():
                    return bt.target
            if isinstance(node, UnaryOperation) and node.operator == "*":
                pt = peekType(node.operand)
                if pt is not None and pt.isPointer():
                    return pt.target
            if isinstance(node, MemberAccess):
                bt = peekType(node.base)
                st = None
                if node.arrow:
                    if bt is not None and bt.isPointer() and bt.target.isStruct():
                        st = bt.target
                elif bt is not None and bt.isStruct():
                    st = bt
                if st is not None and st.members is not None:
                    for memberName, memberType, _ in st.members:
                        if memberName == node.memberName:
                            return memberType
            # Conditional expression: peek at the branches
            if isinstance(node, ConditionalExpression):
                lt = peekType(node.trueExpression)
                if is32Bit(lt):
                    return lt
                return peekType(node.falseExpression)
            return t
        leftPre = peekType(self.expressionLeft)
        rightPre = peekType(self.expressionRight)
        if is32Bit(leftPre) or is32Bit(rightPre):
            return self.generate32(scope)

        code = self.expressionLeft.generate(scope)
        leftType = decay(self.expressionLeft.ctype)
        code += "\n\tpush r0"
        rightCode = self.expressionRight.generate(scope)
        rightType = decay(self.expressionRight.ctype)

        self.ctype = INT

        if op == "+":
            scaleLeftAfterPop = 1
            if leftType.isPointer() and rightType.isInteger():
                rightCode = genScaledIndex(rightCode, max(leftType.target.size, 1))
                self.ctype = leftType
            elif leftType.isInteger() and rightType.isPointer():
                scaleLeftAfterPop = max(rightType.target.size, 1)
                self.ctype = rightType
            code += rightCode + "\n\tpop r1"
            if scaleLeftAfterPop != 1:
                code += "\n\tlmuli r1,#" + str(scaleLeftAfterPop)
            code += "\n\tadd r0,r1,r0"
            return code

        if op == "-":
            if leftType.isPointer() and rightType.isInteger():
                rightCode = genScaledIndex(rightCode, max(leftType.target.size, 1))
                self.ctype = leftType
                code += rightCode + "\n\tpop r1\n\tsub r0,r1,r0"
                return code
            if leftType.isPointer() and rightType.isPointer():
                code += rightCode + "\n\tpop r1\n\tsub r0,r1,r0"
                scale = max(leftType.target.size, 1)
                if scale != 1:
                    cg.neededHelpers.add("__sdiv")
                    code += "\n\tpush r0" \
                          + "\n\tin r0,#" + str(scale) \
                          + "\n\tpop r1" \
                          + "\n\tldpc r2" \
                          + "\n\taddi r2,#4" \
                          + "\n\tpush r2" \
                          + "\n\tjmp __sdiv"
                self.ctype = INT
                return code

        code += rightCode + "\n\tpop r1"

        unsignedDiv = (op in ("/", "%") and
                       leftType.isInteger() and rightType.isInteger() and
                       getattr(leftType, 'isUnsigned', False))

        if op in binaryOpInstructions:
            if op in ("/", "%"):
                if unsignedDiv:
                    code += "\n\t" + binaryOpInstructions[op] + " r0,r1,r0"
                else:
                    helper = "__sdiv" if op == "/" else "__smod"
                    cg.neededHelpers.add(helper)
                    code += "\n\tldpc r2" \
                          + "\n\taddi r2,#4" \
                          + "\n\tpush r2" \
                          + "\n\tjmp " + helper
            else:
                code += "\n\t" + binaryOpInstructions[op] + " r0,r1,r0"
            if op not in ("+", "-") and (leftType.isPointer() or rightType.isPointer()):
                warn("pointer used in arithmetic '%s'" % op, self.line)
            if leftType.isPointer():
                self.ctype = leftType
        elif op in comparisonFlags:
            unsignedCmp = (op in ("<", ">") and
                           leftType.isInteger() and rightType.isInteger() and
                           getattr(leftType, 'isUnsigned', False))
            if unsignedCmp:
                code += "\n\txori r0,#0x8000\n\txori r1,#0x8000"
            mask, shift = comparisonFlags[op]
            code += "\n\tcmp r1,r0" \
                  + "\n\tmov r0,state" \
                  + "\n\tandi r0,#" + str(mask) \
                  + "\n\tshri r0,#" + str(shift)
        elif op == "<=" or op == ">=":
            unsignedCmp = (leftType.isInteger() and rightType.isInteger() and
                           getattr(leftType, 'isUnsigned', False))
            if unsignedCmp:
                code += "\n\txori r0,#0x8000\n\txori r1,#0x8000"
            mask, shift = comparisonFlags["<" if op == "<=" else ">"]
            code += "\n\tcmp r1,r0" \
                  + "\n\tmov r0,state" \
                  + "\n\tmov r1,r0" \
                  + "\n\tandi r0,#" + str(mask) \
                  + "\n\tshri r0,#" + str(shift) \
                  + "\n\tandi r1,#16" \
                  + "\n\tshri r1,#4" \
                  + "\n\tor r0,r1,r0"
        else:
            fail("unknown binary operator " + op, self.line)
        return code

    def generateLogic(self, scope):
        self.ctype = INT
        labelSkip = createUniqueLabel()
        labelEnd = createUniqueLabel()
        if self.operator == "&&":
            code = self.expressionLeft.generate(scope)
            code += genTruthTest(self.expressionLeft) \
                 + "\n\tjeq " + labelSkip \
                 + self.expressionRight.generate(scope)
            code += genTruthTest(self.expressionRight) \
                 + "\n\tjeq " + labelSkip \
                 + "\n\tin r0,#1" \
                 + "\n\tjmp " + labelEnd \
                 + "\n" + labelSkip + ":" \
                 + "\n\tin r0,#0" \
                 + "\n" + labelEnd + ":"
            return code
        code = self.expressionLeft.generate(scope)
        code += genTruthTest(self.expressionLeft) \
             + "\n\tjneq " + labelSkip \
             + self.expressionRight.generate(scope)
        code += genTruthTest(self.expressionRight) \
             + "\n\tjneq " + labelSkip \
             + "\n\tin r0,#0" \
             + "\n\tjmp " + labelEnd \
             + "\n" + labelSkip + ":" \
             + "\n\tin r0,#1" \
             + "\n" + labelEnd + ":"
        return code

    # Float operation → C helper name
    _floatHelpers = {"+": "f32_add", "-": "f32_sub", "*": "f32_mul", "/": "f32_div"}

    def _generateFloatOp(self, scope, leftCode, leftType, rightCode, rightType):
        """Generate float arithmetic/comparison using C runtime helpers.
        C calling convention: push rightmost argument first."""
        op = self.operator
        if op not in self._floatHelpers and \
                op not in ("==", "!=", "<", ">", "<=", ">="):
            fail("invalid operands to binary %s (have float)" % op, self.line)

        # Generate left, save; generate right; rearrange so left is on top (pushed last)
        code = leftCode
        code += "\n\tpush r1\n\tpush r0"      # save left
        code += rightCode
        code += "\n\tpush r1\n\tpush r0"      # save right
        code += "\n\tpop r2\n\tpop r3"        # right → er1
        code += "\n\tpop r0\n\tpop r1"        # left → er0
        code += "\n\tpush r3\n\tpush r2"      # push right first (lo, hi)
        code += "\n\tpush r1\n\tpush r0"      # push left second (lo, hi) → left on top

        if op in self._floatHelpers:
            code += "\n\tcall " + self._floatHelpers[op]
            code += "\n\tpop\n\tpop\n\tpop\n\tpop"
            self.ctype = FLOAT
            return code

        # Comparison: call f32_cmp
        code += "\n\tcall f32_cmp"
        code += "\n\tpop\n\tpop\n\tpop\n\tpop"
        if op == "==":
            code += "\n\tcmp r0,#0\n\tmov r0,state\n\tandi r0,#16\n\tshri r0,#4"
        elif op == "!=":
            code += "\n\tcmp r0,#0\n\tmov r0,state\n\tandi r0,#32\n\tshri r0,#5"
        elif op == "<":
            code += "\n\tpush r0\n\tin r0,#-1\n\tpop r1\n\tcmp r1,r0\n\tmov r0,state\n\tandi r0,#16\n\tshri r0,#4"
        elif op == ">":
            code += "\n\tpush r0\n\tin r0,#0\n\tpop r1\n\tcmp r1,r0\n\tmov r0,state\n\tandi r0,#64\n\tshri r0,#6"
        elif op == "<=":
            code += "\n\tpush r0\n\tin r0,#1\n\tpop r1\n\tcmp r1,r0\n\tmov r0,state\n\tandi r0,#32\n\tshri r0,#5"
        elif op == ">=":
            code += "\n\tpush r0\n\tin r0,#-1\n\tpop r1\n\tcmp r1,r0\n\tmov r0,state\n\tandi r0,#32\n\tshri r0,#5"
        self.ctype = INT
        return code

    # 32-bit long operation map
    _longOp32 = {"+": "add32", "-": "sub32", "*": "lmul32",
                 "/": "div32", "%": "mod32",
                 "&": "and32", "|": "or32", "^": "xor32",
                 "<<": "shl32", ">>": "shr32"}
    _longCmpMask = {"==": (16, 4), "!=": (32, 5), "<": (4, 2), ">": (64, 6)}

    def _genLongOperand(self, node, scope):
        """Generate a 32-bit operand without promotion. For Constants,
        emit full 32-bit. Returns (code, ctype)."""
        if isinstance(node, Constant):
            return emitLongConstant(node.value), LONG
        if isinstance(node, FloatConstant):
            c = node.generate(scope)
            return c, FLOAT
        code = node.generate(scope)
        return code, node.ctype

    def _toFloatOperand(self, node, code, ctype):
        """Make a generated operand a float: fold int constants, convert
        int values with f32_from_int, pass long bits through."""
        if isFloat(ctype):
            return code
        if isinstance(node, Constant):
            return emitLongConstant(floatToBinary32(float(node.value)))
        return emitConversion(code, ctype, FLOAT)

    def generate32(self, scope):
        """Generate 32-bit code for long or float operands."""
        op = self.operator
        leftCode, leftType = self._genLongOperand(self.expressionLeft, scope)
        rightCode, rightType = self._genLongOperand(self.expressionRight, scope)

        floatOp = isFloat(leftType) or isFloat(rightType)
        if floatOp:
            leftCode = self._toFloatOperand(self.expressionLeft, leftCode, leftType)
            rightCode = self._toFloatOperand(self.expressionRight, rightCode, rightType)
            return self._generateFloatOp(scope, leftCode, FLOAT, rightCode, FLOAT)

        # promote 16-bit operands to long (zero-extend)
        if not is32Bit(leftType):
            leftCode += "\n\tmov r1,r0\n\tin r0,#0"
        if not is32Bit(rightType):
            rightCode += "\n\tmov r1,r0\n\tin r0,#0"

        unsignedDiv = ((op == "/" or op == "%") and
                       (getattr(leftType, 'isUnsigned', False) or
                        getattr(rightType, 'isUnsigned', False)))

        # Signed div/mod: dividend (left) in er0, divisor (right) in er1
        if op in ("/", "%") and not unsignedDiv:
            code = leftCode \
                 + "\n\tpush r1" \
                 + "\n\tpush r0"
            code += rightCode      # divisor in er0
            code += "\n\tmov r3,r1" \
                  + "\n\tmov r2,r0"  # divisor → er1
            code += "\n\tpop r0" \
                  + "\n\tpop r1"     # dividend → er0
            # Save divisor before trampoline destroys r2
            helper = "__sdiv32" if op == "/" else "__smod32"
            cg.neededHelpers.add(helper)
            code += "\n\tpush r3" \
                  + "\n\tpush r2" \
                  + "\n\tldpc r2" \
                  + "\n\taddi r2,#4" \
                  + "\n\tpush r2" \
                  + "\n\tjmp " + helper
            self.ctype = LONG
            return code

        # For shifts (<<, >>): left is the value to shift, right is the count.
        # shr32/shl32 do: er0 = er0 op er1.  So we want left in er0, right in er1.
        if op in ("<<", ">>"):
            code = leftCode \
                 + "\n\tpush r1" \
                 + "\n\tpush r0"
            code += rightCode   # shift count in er0
            code += "\n\tmov r3,r1" \
                  + "\n\tmov r2,r0"  # shift count → er1
            code += "\n\tpop r0" \
                  + "\n\tpop r1"     # value → er0
            code += "\n\t" + self._longOp32[op] + " er0,er1"
            self.ctype = LONG
            return code

        # Standard: left in er1 (r2:r3), right in er0 (r0:r1)
        code = leftCode \
             + "\n\tpush r1" \
             + "\n\tpush r0"
        code += rightCode
        code += "\n\tpop r2" \
              + "\n\tpop r3"

        # Unsigned div/mod: er1 / er0, result in er1, move to er0
        if op in ("/", "%"):
            code += "\n\t" + self._longOp32[op] + " er1,er0" \
                  + "\n\tmov r0,r2" \
                  + "\n\tmov r1,r3"
            self.ctype = LONG
            return code

        # Subtraction: er1 - er0, result in er1, move to er0
        if op == "-":
            code += "\n\tsub32 er1,er0" \
                  + "\n\tmov r0,r2" \
                  + "\n\tmov r1,r3"
            self.ctype = LONG
            return code

        op32 = self._longOp32.get(op)
        if op32:
            code += "\n\t" + op32 + " er0,er1"
            self.ctype = LONG
            return code

        # Comparisons: cmp er1,er0 = cmp(left, right)
        cmpInfo = self._longCmpMask.get(op)
        if cmpInfo:
            mask, shift = cmpInfo
            code += "\n\tcmp32 er1,er0" \
                  + "\n\tmov r0,state" \
                  + "\n\tandi r0,#" + str(mask) \
                  + "\n\tshri r0,#" + str(shift)
            self.ctype = INT
            return code

        if op == "<=" or op == ">=":
            baseOp = "<" if op == "<=" else ">"
            mask, shift = self._longCmpMask[baseOp]
            code += "\n\tcmp32 er1,er0" \
                  + "\n\tmov r0,state" \
                  + "\n\tmov r1,r0" \
                  + "\n\tandi r0,#" + str(mask) \
                  + "\n\tshri r0,#" + str(shift) \
                  + "\n\tandi r1,#16" \
                  + "\n\tshri r1,#4" \
                  + "\n\tor r0,r1,r0"
            self.ctype = INT
            return code

        fail("unsupported 32-bit operator " + op, self.line)


class ConditionalExpression(Node):
    def __init__(self, conditionExpression, trueExpression, falseExpression):
        self.conditionExpression = conditionExpression
        self.trueExpression = trueExpression
        self.falseExpression = falseExpression

    def generate(self, scope):
        labelFalse = createUniqueLabel()
        labelEnd = createUniqueLabel()
        code = self.conditionExpression.generate(scope)
        code += genTruthTest(self.conditionExpression) \
             + "\n\tjeq " + labelFalse \
             + self.trueExpression.generate(scope) \
             + "\n\tjmp " + labelEnd \
             + "\n" + labelFalse + ":" \
             + self.falseExpression.generate(scope) \
             + "\n" + labelEnd + ":"
        self.ctype = decay(self.trueExpression.ctype)
        return code


assignmentOperators = {
    "+=": "+", "-=": "-", "*=": "*", "/=": "/", "%=": "%",
    "<<=": "<<", ">>=": ">>", "&=": "&", "^=": "^", "|=": "|"}


class Assignment(Node):
    def __init__(self, operator, target, valueExpression):
        self.operator = operator
        self.target = target
        self.valueExpression = valueExpression

    def generate(self, scope):
        if self.operator == "=":
            # Generate the target address first so the target type is known
            # (the emitted code still evaluates the value first)
            addressCode = self.target.generateAddress(scope)
            targetType = self.target.ctype
            # For 32-bit targets with constant values, emit full 32-bit
            if is32Bit(targetType) and isinstance(self.valueExpression, (Constant, FloatConstant)):
                if isinstance(self.valueExpression, FloatConstant):
                    valueCode = self.valueExpression.generate(scope)
                elif isFloat(targetType):
                    # float f; f = 5;  → fold the int→float conversion
                    valueCode = emitLongConstant(
                        floatToBinary32(float(self.valueExpression.value)))
                else:
                    valueCode = emitLongConstant(self.valueExpression.value)
                valueType = targetType
            else:
                valueCode = self.valueExpression.generate(scope)
                valueType = self.valueExpression.ctype
            self.ctype = targetType

            if targetType.isConst:
                warn("assignment to const-qualified variable", self.line)

            if targetType.isStruct():
                if not (valueType.isStruct() and valueType.size == targetType.size):
                    fail("incompatible types in struct assignment", self.line)
                code = valueCode \
                     + "\n\tpush r0" \
                     + addressCode \
                     + "\n\tmov r2,r0" \
                     + "\n\tpop r1"
                for i in range(targetType.size):
                    code += "\n\tldo r0,*r1,#" + str(i) \
                          + "\n\tstoo *r2,#" + str(i) + ",r0"
                code += "\n\tmov r0,r2"
                return code

            if targetType.isArray():
                fail("cannot assign to an array", self.line)
            valueCode = emitConversion(valueCode, valueType, targetType)
            # plain variable target: store directly, no address juggling
            symbol = simpleVarSymbol(self.target, scope)
            if symbol is not None:
                return valueCode + storeSimpleVar(symbol)
            if is32Bit(targetType):
                # 32-bit assignment: value is in r0:r1, store both words
                # (int→float converts, int→long zero-extends)
                return valueCode \
                     + "\n\tpush r0" \
                     + "\n\tpush r1" \
                     + addressCode \
                     + "\n\tmov r2,r0" \
                     + "\n\tpop r1" \
                     + "\n\tpop r0" \
                     + "\n\tstor *r2,r0" \
                     + "\n\tstoo *r2,#1,r1"
            # 16-bit target: convert a float value (f32_to_int) or take
            # the low word of a long
            return valueCode \
                 + "\n\tpush r0" \
                 + addressCode \
                 + "\n\tmov r1,r0" \
                 + "\n\tpop r0" \
                 + "\n\tstor *r1,r0"

        # compound assignment
        op = assignmentOperators[self.operator]
        addressCode = self.target.generateAddress(scope)
        targetType = self.target.ctype
        self.ctype = targetType
        if targetType.isConst:
            warn("assignment to const-qualified variable", self.line)
        if not targetType.isScalar():
            fail("invalid target for " + self.operator, self.line)

        if is32Bit(targetType):
            # long/float compound assignment: desugar to  target = target op
            # value.  (The target lvalue is evaluated twice; fine for the
            # simple lvalues this compiler supports.)
            operation = BinaryOperator(op, self.target, self.valueExpression)
            operation.line = self.line
            assignment = Assignment("=", self.target, operation)
            assignment.line = self.line
            code = assignment.generate(scope)
            self.ctype = assignment.ctype
            return code

        valueCode = self.valueExpression.generate(scope)
        if targetType.isPointer() and op in ("+", "-"):
            valueCode = genScaledIndex(valueCode, max(targetType.target.size, 1))
        elif targetType.isPointer():
            warn("pointer used in arithmetic '%s'" % self.operator, self.line)

        if op in ("/", "%"):
            helper = "__sdiv" if op == "/" else "__smod"
            cg.neededHelpers.add(helper)
            operation = ("\n\tldpc r2"
                         "\n\taddi r2,#4"
                         "\n\tpush r2"
                         "\n\tjmp " + helper)
        else:
            operation = "\n\t" + binaryOpInstructions[op] + " r0,r1,r0"

        # plain variable target: value in r0, old value in r1, store back
        symbol = simpleVarSymbol(self.target, scope)
        if symbol is not None:
            return valueCode \
                 + loadSimpleVar(symbol, "r1") \
                 + operation \
                 + storeSimpleVar(symbol)

        return addressCode \
             + "\n\tpush r0" \
             + "\n\tldr r0,*r0" \
             + "\n\tpush r0" \
             + valueCode \
             + "\n\tpop r1" \
             + operation \
             + "\n\tpop r1" \
             + "\n\tstor *r1,r0"


class Expression(Node):
    """comma separated expression list"""
    def __init__(self, expressionList):
        self.expressionList = expressionList

    def generate(self, scope):
        code = ""
        for expression in self.expressionList:
            code += expression.generate(scope)
        self.ctype = self.expressionList[-1].ctype
        return code

    def generateAddress(self, scope):
        if len(self.expressionList) == 1:
            code = self.expressionList[0].generateAddress(scope)
            self.ctype = self.expressionList[0].ctype
            return code
        fail("expression is not an lvalue", self.line)


############################### AST: STATEMENTS ###############################

class InlineAssembler(Node):
    def __init__(self, assembler):
        self.assembler = assembler

    def generate(self, scope):
        return self.assembler


def generateDiscarded(expression, scope):
    """Generate an expression whose value is not used.  A post-inc/dec
    then doesn't need to preserve the old value, so emit it as the
    cheaper pre-inc/dec."""
    if isinstance(expression, PostIncDec):
        pre = PreIncDec(expression.operand, expression.operator)
        pre.line = expression.line
        code = pre.generate(scope)
        expression.ctype = pre.ctype
        return code
    return expression.generate(scope)


class ExpressionStatement(Node):
    def __init__(self, expression):
        self.expression = expression

    def generate(self, scope):
        if self.expression is None:
            return ""
        return generateDiscarded(self.expression, scope)


class CompoundStatement(Node):
    def __init__(self, declarationList, statementList):
        self.declarationList = declarationList
        self.statementList = statementList

    def generate(self, scope):
        blockScope = Scope(scope)
        ctx = cg.currentFunction
        savedCursor = ctx.cur
        code = ""
        for declaration in self.declarationList:
            code += declaration.generate(blockScope)
        for statement in self.statementList:
            code += statement.generate(blockScope)
        ctx.cur = savedCursor       # siblings may reuse the slots
        return code


class ConditionalStatement(Node):
    def __init__(self, expression, thenStatement, elseStatement=None):
        self.expression = expression
        self.thenStatement = thenStatement
        self.elseStatement = elseStatement

    def generate(self, scope):
        labelElse = createUniqueLabel()
        labelEnd = createUniqueLabel()
        code = self.expression.generate(scope)
        code += genTruthTest(self.expression) \
             + "\n\tjeq " + labelElse \
             + self.thenStatement.generate(scope) \
             + "\n\tjmp " + labelEnd \
             + "\n" + labelElse + ":"
        if self.elseStatement is not None:
            code += self.elseStatement.generate(scope)
        code += "\n" + labelEnd + ":"
        return code


class WhileLoop(Node):
    def __init__(self, expression, statement):
        self.expression = expression
        self.statement = statement

    def generate(self, scope):
        labelStart = createUniqueLabel()
        labelEnd = createUniqueLabel()
        ctx = cg.currentFunction
        ctx.breakLabels.append(labelEnd)
        ctx.continueLabels.append(labelStart)
        code = "\n" + labelStart + ":" \
             + self.expression.generate(scope)
        code += genTruthTest(self.expression) \
             + "\n\tjeq " + labelEnd \
             + self.statement.generate(scope) \
             + "\n\tjmp " + labelStart \
             + "\n" + labelEnd + ":"
        ctx.breakLabels.pop()
        ctx.continueLabels.pop()
        return code


class DoWhileLoop(Node):
    def __init__(self, statement, expression):
        self.statement = statement
        self.expression = expression

    def generate(self, scope):
        labelStart = createUniqueLabel()
        labelCondition = createUniqueLabel()
        labelEnd = createUniqueLabel()
        ctx = cg.currentFunction
        ctx.breakLabels.append(labelEnd)
        ctx.continueLabels.append(labelCondition)
        code = "\n" + labelStart + ":" \
             + self.statement.generate(scope) \
             + "\n" + labelCondition + ":" \
             + self.expression.generate(scope)
        code += genTruthTest(self.expression) \
             + "\n\tjneq " + labelStart \
             + "\n" + labelEnd + ":"
        ctx.breakLabels.pop()
        ctx.continueLabels.pop()
        return code


class ForLoop(Node):
    def __init__(self, initialExpression, conditionalExpression, postExpression, statement):
        self.initialExpression = initialExpression
        self.conditionalExpression = conditionalExpression
        self.postExpression = postExpression
        self.statement = statement

    def generate(self, scope):
        labelCondition = createUniqueLabel()
        labelContinue = createUniqueLabel()
        labelEnd = createUniqueLabel()
        ctx = cg.currentFunction
        ctx.breakLabels.append(labelEnd)
        ctx.continueLabels.append(labelContinue)
        code = ""
        if self.initialExpression is not None:
            code += self.initialExpression.generate(scope)
        code += "\n" + labelCondition + ":"
        if self.conditionalExpression is not None:
            code += self.conditionalExpression.generate(scope)
            code += genTruthTest(self.conditionalExpression) \
                  + "\n\tjeq " + labelEnd
        code += self.statement.generate(scope) \
              + "\n" + labelContinue + ":"
        if self.postExpression is not None:
            code += generateDiscarded(self.postExpression, scope)
        code += "\n\tjmp " + labelCondition \
              + "\n" + labelEnd + ":"
        ctx.breakLabels.pop()
        ctx.continueLabels.pop()
        return code


class SwitchStatement(Node):
    def __init__(self, expression, statement):
        self.expression = expression
        self.statement = statement

    def generate(self, scope):
        labelEnd = createUniqueLabel()
        ctx = cg.currentFunction
        ctx.breakLabels.append(labelEnd)

        cases = []
        defaults = []
        collectCases(self.statement, cases, defaults)
        if len(defaults) > 1:
            fail("multiple default labels in one switch", self.line)

        seen = set()
        code = self.expression.generate(scope)
        for caseNode in cases:
            caseNode.switchLabel = createUniqueLabel()
            value = evalConst(caseNode.constantExpression, "case label")
            if value in seen:
                fail("duplicate case value %d" % value, caseNode.line)
            seen.add(value)
            code += "\n\tcmp r0,#" + str(value) \
                  + "\n\tjeq " + caseNode.switchLabel
        if defaults:
            defaults[0].switchLabel = createUniqueLabel()
            code += "\n\tjmp " + defaults[0].switchLabel
        else:
            code += "\n\tjmp " + labelEnd

        code += self.statement.generate(scope)
        code += "\n" + labelEnd + ":"
        ctx.breakLabels.pop()
        return code


def collectCases(node, cases, defaults):
    if node is None or isinstance(node, SwitchStatement):
        return
    if isinstance(node, CaseLabel):
        (defaults if node.constantExpression is None else cases).append(node)
        collectCases(node.statement, cases, defaults)
    elif isinstance(node, CompoundStatement):
        for statement in node.statementList:
            collectCases(statement, cases, defaults)
    elif isinstance(node, ConditionalStatement):
        collectCases(node.thenStatement, cases, defaults)
        collectCases(node.elseStatement, cases, defaults)
    elif isinstance(node, (WhileLoop, ForLoop)):
        collectCases(node.statement, cases, defaults)
    elif isinstance(node, DoWhileLoop):
        collectCases(node.statement, cases, defaults)
    elif isinstance(node, GotoLabel):
        collectCases(node.statement, cases, defaults)


class CaseLabel(Node):
    def __init__(self, constantExpression, statement):
        self.constantExpression = constantExpression  # None for default:
        self.statement = statement
        self.switchLabel = None

    def generate(self, scope):
        if self.switchLabel is None:
            fail("case/default label outside of a switch", self.line)
        return "\n" + self.switchLabel + ":" + self.statement.generate(scope)


class GotoLabel(Node):
    def __init__(self, name, statement):
        self.name = name
        self.statement = statement

    def generate(self, scope):
        label = "__U_" + cg.currentFunction.name + "_" + self.name
        if label in cg.currentFunction.usedLabels:
            fail("duplicate label '%s'" % self.name, self.line)
        cg.currentFunction.usedLabels.add(label)
        return "\n" + label + ":" + self.statement.generate(scope)


class Goto(Node):
    def __init__(self, name):
        self.name = name

    def generate(self, scope):
        return "\n\tjmp __U_" + cg.currentFunction.name + "_" + self.name


class Break(Node):
    def generate(self, scope):
        if not cg.currentFunction.breakLabels:
            fail("break outside of loop or switch", self.line)
        return "\n\tjmp " + cg.currentFunction.breakLabels[-1]


class Continue(Node):
    def generate(self, scope):
        if not cg.currentFunction.continueLabels:
            fail("continue outside of loop", self.line)
        return "\n\tjmp " + cg.currentFunction.continueLabels[-1]


class Return(Node):
    def __init__(self, expression):
        self.expression = expression

    def generate(self, scope):
        code = ""
        returnType = cg.currentFunction.returnType
        returns32 = is32Bit(returnType)
        if self.expression is not None:
            if isFloat(returnType) and isinstance(self.expression, Constant):
                # return 5; from a float function: fold the conversion
                code += emitLongConstant(
                    floatToBinary32(float(self.expression.value)))
            elif is32Bit(returnType) and isinstance(self.expression, Constant):
                # full 32-bit constant (return 0x7FFFFFFF; from a long fn)
                code += emitLongConstant(self.expression.value)
            else:
                code += self.expression.generate(scope)
                exprType = self.expression.ctype
                if exprType.isStruct():
                    fail("returning structs by value is not supported",
                         self.line)
                # int <-> float conversion / int -> long promotion
                code = emitConversion(code, exprType, returnType)
        elif returnType.kind != "void":
            warn("return without value in non-void function '%s'"
                 % cg.currentFunction.name, self.line)
            if returns32:
                code += "\n\tin r0,#0\n\tin r1,#0"
        if returns32:
            # 'ret' uses r1 (pop r1; jmpr r1), which destroys the lo word.
            # Use r2 for the return address instead to preserve er0 (r0:r1).
            code += "\n\tmov sp,r15" \
                  + "\n\tpop r15" \
                  + "\n\tpop r2" \
                  + "\n\tjmpr r2"
        else:
            code += "\n\tmov sp,r15" \
                  + "\n\tpop r15" \
                  + "\n\tret"
        return code


############################### AST: DECLARATIONS ###############################

def flattenInitializer(ctype, initializer, line):
    """Yield (offset, expressionNode or None) pairs covering ctype.
    Trailing uncovered words are zero (None entries are emitted for them
    only when an initializer list was given)."""
    result = []

    def walk(ctype, initializer, base):
        if isinstance(initializer, list):
            if ctype.isArray():
                element = ctype.element
                if len(initializer) > ctype.length:
                    fail("too many initializers", line)
                for i, item in enumerate(initializer):
                    walk(element, item, base + i * element.size)
                for i in range(len(initializer) * element.size, ctype.size):
                    result.append((base + i, None))
            elif ctype.isStruct() and not ctype.isUnion:
                if ctype.members is None:
                    fail("initializing incomplete struct", line)
                if len(initializer) > len(ctype.members):
                    fail("too many initializers", line)
                for i, item in enumerate(initializer):
                    name, memberType, offset = ctype.members[i]
                    walk(memberType, item, base + offset)
                covered = sum(m[1].size for m in ctype.members[:len(initializer)])
                for i in range(covered, ctype.size):
                    result.append((base + i, None))
            elif ctype.isStruct() and ctype.isUnion:
                if len(initializer) != 1:
                    fail("union initializer must have exactly one element", line)
                walk(ctype.members[0][1], initializer[0], base)
            elif len(initializer) == 1:
                walk(ctype, initializer[0], base)
            else:
                fail("scalar initializer with too many elements", line)
        elif isinstance(initializer, StringLiteral) and ctype.isArray():
            text = initializer.text
            if ctype.length is not None and len(text) + 1 > ctype.size:
                fail("string too long for array", line)
            for i, char in enumerate(text):
                result.append((base + i, Constant(ord(char))))
            for i in range(len(text), ctype.size):
                result.append((base + i, None))
        else:
            if not ctype.isScalar():
                fail("invalid initializer", line)
            result.append((base, initializer))

    walk(ctype, initializer, 0)
    return result


def completeArrayLength(ctype, initializer, line):
    """int a[] = {...} / char s[] = "..." - infer the length"""
    if ctype.isArray() and ctype.length is None:
        if isinstance(initializer, list):
            length = len(initializer)
        elif isinstance(initializer, StringLiteral):
            length = len(initializer.text) + 1
        else:
            fail("cannot determine array size", line)
        return ArrayType(ctype.element, length)
    return ctype


class Declaration(Node):
    """one declaration with a list of (name, type, initializer) declarators"""
    def __init__(self, storageClass, declarators):
        self.storageClass = storageClass
        self.declarators = declarators

    def generate(self, scope):
        # local scope declaration
        code = ""
        ctx = cg.currentFunction
        for name, ctype, initializer in self.declarators:
            if ctype.isFunction():
                symbol = declareFunction(name, ctype, self.line)
                continue
            ctype = completeArrayLength(ctype, initializer, self.line)
            if ctype.size == 0:
                fail("variable '%s' has incomplete type" % name, self.line)

            if self.storageClass == "static":
                address = cg.allocGlobal(ctype.size)
                symbol = Symbol(name, ctype, "global", address=address)
                scope.declare(symbol, self.line)
                if initializer is not None:
                    recordGlobalInit(ctype, address, initializer, self.line)
                continue

            offset = ctx.allocLocal(ctype.size)
            symbol = Symbol(name, ctype, "local", offset=offset)
            scope.declare(symbol, self.line)

            if initializer is None:
                continue
            if isinstance(initializer, (list, StringLiteral)) and not ctype.isScalar():
                for wordOffset, expression in flattenInitializer(ctype, initializer, self.line):
                    if expression is None:
                        code += "\n\tin r0,#0"
                    else:
                        code += expression.generate(scope)
                        if expression.ctype.isStruct():
                            fail("nested struct value in initializer list", self.line)
                    code += "\n\tstoo *r15,#" + str((offset + wordOffset) & 0xffff) + ",r0"
            elif ctype.isStruct():
                code += initializer.generate(scope)     # struct address in r0
                if not initializer.ctype.isStruct():
                    fail("invalid struct initializer", self.line)
                code += "\n\tmov r1,r0"
                for i in range(ctype.size):
                    code += "\n\tldo r0,*r1,#" + str(i) \
                          + "\n\tstoo *r15,#" + str((offset + i) & 0xffff) + ",r0"
            elif is32Bit(ctype) and isinstance(initializer, (Constant, FloatConstant)):
                if isinstance(initializer, FloatConstant):
                    code += initializer.generate(scope)
                elif isFloat(ctype):
                    # float f = 5;  → fold the int→float conversion
                    code += emitLongConstant(
                        floatToBinary32(float(initializer.value)))
                else:
                    code += emitLongConstant(initializer.value)
                code += "\n\tstoo *r15,#" + str(offset & 0xffff) + ",r0" \
                      + "\n\tstoo *r15,#" + str((offset + 1) & 0xffff) + ",r1"
            elif is32Bit(ctype):
                # Try constant folding for unary-minus etc.
                try:
                    if containsWideConstant(initializer):
                        raise NotConstant()
                    val = evalConstInner(initializer)
                    if isFloat(ctype):
                        val = floatToBinary32(float(val))
                    code += emitLongConstant(val) \
                          + "\n\tstoo *r15,#" + str(offset & 0xffff) + ",r0" \
                          + "\n\tstoo *r15,#" + str((offset + 1) & 0xffff) + ",r1"
                except NotConstant:
                    # Non-constant — convert/promote to the declared type
                    initCode = initializer.generate(scope)
                    initCode = emitConversion(initCode, initializer.ctype, ctype)
                    code += initCode \
                          + "\n\tstoo *r15,#" + str(offset & 0xffff) + ",r0" \
                          + "\n\tstoo *r15,#" + str((offset + 1) & 0xffff) + ",r1"
            else:
                initCode = initializer.generate(scope)
                initCode = emitConversion(initCode, initializer.ctype, ctype)
                code += initCode \
                      + "\n\tstoo *r15,#" + str(offset & 0xffff) + ",r0"
        return code


def declareFunction(name, functionType, line):
    if name in cg.functions:
        existing = cg.functions[name].ctype
        if existing.params is not None and functionType.params is not None \
                and len(existing.params) != len(functionType.params):
            fail("conflicting declarations of function '%s'" % name, line)
        if existing.params is None:
            cg.functions[name].ctype = functionType
        return cg.functions[name]
    symbol = Symbol(name, functionType, "function")
    cg.functions[name] = symbol
    return symbol


def recordGlobalInit(ctype, address, initializer, line):
    if isinstance(initializer, (list, StringLiteral)) and not ctype.isScalar():
        for wordOffset, expression in flattenInitializer(ctype, initializer, line):
            if expression is None:
                continue                        # RAM starts zeroed
            cg.globalInits.append((address + wordOffset,
                                   constInitValue(expression, line)))
    else:
        if not ctype.isScalar():
            fail("invalid global initializer", line)
        if is32Bit(ctype):
            value = constInitValue32(ctype, initializer, line)
            cg.globalInits.append((address, (value >> 16) & 0xffff))
            cg.globalInits.append((address + 1, value & 0xffff))
        else:
            value = constInitValue(initializer, line)
            cg.globalInits.append((address, value))


def constInitValue(node, line):
    """constant initializer: number, string address or global address"""
    if isinstance(node, StringLiteral):
        return ("str", cg.internString(node.text))
    if isinstance(node, UnaryOperation) and node.operator == "&" \
            and isinstance(node.operand, VariableAccess):
        return ("addr", node.operand.name, line)
    if isinstance(node, VariableAccess) and node.name not in enumConstants:
        # array/function name used as address constant
        return ("addr", node.name, line)
    try:
        return evalConstInner(node) & 0xffff
    except NotConstant:
        fail("global initializer is not constant", line)


def constInitValue32(ctype, node, line):
    """constant initializer for a long or float global: full 32-bit value"""
    if isinstance(node, FloatConstant):
        return node.bits
    try:
        value = evalConstInner(node)
    except NotConstant:
        fail("global initializer is not constant", line)
    if isFloat(ctype):
        return floatToBinary32(float(value))
    return value & 0xffffffff


class GlobalDeclaration(Node):
    def __init__(self, storageClass, declarators):
        self.storageClass = storageClass
        self.declarators = declarators

    def register(self):
        for name, ctype, initializer in self.declarators:
            if ctype.isFunction():
                declareFunction(name, ctype, self.line)
                continue
            ctype = completeArrayLength(ctype, initializer, self.line)
            existing = cg.globalScope.symbols.get(name)
            if existing is not None:
                if initializer is None:
                    continue                    # tentative redeclaration
                if existing.hasInit:
                    fail("redefinition of '%s'" % name, self.line)
                # tentative/extern declaration earlier, real definition now
                if ctype.size > existing.ctype.size:
                    fail("conflicting types for '%s'" % name, self.line)
                existing.ctype = ctype
                existing.hasInit = True
                recordGlobalInit(ctype, existing.address, initializer, self.line)
                continue
            if ctype.size == 0:
                fail("global '%s' has incomplete type" % name, self.line)
            address = cg.allocGlobal(ctype.size)
            symbol = Symbol(name, ctype, "global", address=address)
            cg.globalScope.declare(symbol, self.line)
            if initializer is not None:
                symbol.hasInit = True
                recordGlobalInit(ctype, address, initializer, self.line)

    def generate(self, scope):
        return ""


class FunctionDefinition(Node):
    def __init__(self, name, functionType, parameterNames, compoundStatement, naked=False):
        self.name = name
        self.functionType = functionType
        self.parameterNames = parameterNames
        self.compoundStatement = compoundStatement
        self.naked = naked

    def generate(self, scope):
        symbol = declareFunction(self.name, self.functionType, self.line)
        if symbol.defined:
            fail("redefinition of function '%s'" % self.name, self.line)
        symbol.defined = True
        symbol.ctype = self.functionType

        ctx = FunctionContext(self.name, self.functionType.returnType)
        cg.currentFunction = ctx
        functionScope = Scope(cg.globalScope)

        offset = 3
        for parameterName, parameterType in zip(self.parameterNames,
                                                [p[1] for p in self.functionType.params or []]):
            if parameterName is None:
                fail("unnamed parameter in definition of '%s'" % self.name, self.line)
            functionScope.declare(Symbol(parameterName, parameterType, "param",
                                         offset=offset), self.line)
            offset += parameterType.size

        body = self.compoundStatement.generate(functionScope)

        if self.naked:
            code = "\n" + self.name + ":" + body
        else:
            code = "\n" + self.name + ":" \
                 + "\n\tpush r15" \
                 + "\n\tmov r15,sp"
            frameSize = ctx.frameSize()
            if frameSize > 0:
                code += "\n\tldsp r0" \
                      + "\n\tsubi r0,#" + str(frameSize) \
                      + "\n\tstosp r0"
            code += body
            # If the body already ends with a return, skip the default epilogue
            stmts = self.compoundStatement.statementList
            endsWithReturn = stmts and isinstance(stmts[-1], Return)
            if not endsWithReturn:
                if isLong(self.functionType.returnType):
                    code += "\n\tin r0,#0\n\tin r1,#0" \
                          + "\n\tmov sp,r15" \
                          + "\n\tpop r15" \
                          + "\n\tpop r2" \
                          + "\n\tjmpr r2"
                else:
                    code += "\n\tin r0,#0" \
                          + "\n\tmov sp,r15" \
                          + "\n\tpop r15" \
                          + "\n\tret"

        cg.currentFunction = None
        return code


class Program(Node):
    def __init__(self, externalDeclarations):
        self.externalDeclarations = externalDeclarations

    def generate(self):
        # first register all globals and function declarations so that
        # forward references work
        for declaration in self.externalDeclarations:
            if isinstance(declaration, GlobalDeclaration):
                declaration.register()

        functionChunks = {}
        for declaration in self.externalDeclarations:
            if isinstance(declaration, FunctionDefinition):
                functionChunks[declaration.name] = declaration.generate(cg.globalScope)

        if "main" not in functionChunks:
            fail("no 'main' function defined")

        # place strings after the globals
        cg.stringAddresses = []
        cursor = cg.globalCursor
        for text in cg.strings:
            cg.stringAddresses.append(cursor)
            cursor += len(text) + 1
        if cursor > HEAP_BASE:
            fail("globals and strings exceed available data memory")

        # startup code
        startup = "\n\tin r0,#0" \
                + "\n\tmov page,r0" \
                + "\n\tin r0,#0xffff" \
                + "\n\tstosp r0"
        # collect all data initializers, then group identical values so
        # each distinct value is loaded into r0 only once (all writes go
        # to distinct addresses, so reordering is safe)
        initWrites = []             # (address, operand text)
        for address, value in cg.globalInits:
            operand = self.initOperand(value)
            if operand is not None:
                initWrites.append((address, operand))
        for index, text in enumerate(cg.strings):
            address = cg.stringAddresses[index]
            for i, char in enumerate(text + "\0"):
                if ord(char) != 0:
                    initWrites.append((address + i, str(ord(char))))
        groups = {}
        order = []
        for address, operand in initWrites:
            if operand not in groups:
                groups[operand] = []
                order.append(operand)
            groups[operand].append(address)
        for operand in order:
            startup += "\n\tin r0,#" + operand
            for address in groups[operand]:
                startup += "\n\tstod *" + str(address) + ",r0"
        startup += "\n\tcall main" \
                 + "\n__halt:" \
                 + "\n\tjmp __halt"

        code = startup
        for name in functionChunks:
            code += functionChunks[name]
        code += cg.runtimeHelpers()

        # patch string address placeholders
        def patchString(match):
            return str(cg.stringAddresses[int(match.group(1))])
        code = re.sub(r"@STR(\d+)@", patchString, code)

        self.functionChunks = functionChunks
        return code

    def initOperand(self, value):
        """operand text for an 'in r0,#...' initializer load, or None
        for zero values (RAM starts zeroed)"""
        if isinstance(value, tuple):
            if value[0] == "str":
                number = cg.stringAddresses[value[1]]
            else:
                name = value[1]
                symbol = cg.globalScope.symbols.get(name)
                if symbol is not None:
                    number = symbol.address
                elif name in cg.functions:
                    return name             # label operand
                else:
                    fail("unknown symbol '%s' in global initializer" % name, value[2])
        else:
            number = value
        if number == 0:
            return None
        return str(number)


############################### DEAD CODE ELIMINATION ###############################

def eliminateDeadFunctions(code, functionChunks):
    """drop functions that are unreachable from the startup code"""
    referenced = set()
    worklist = ["main"]
    chunkBodies = dict(functionChunks)

    def referencesIn(text):
        names = set()
        for match in re.finditer(r"\b(?:call|jmp|in r\d+,#)\s*([A-Za-z_]\w*)", text):
            names.add(match.group(1))
        return names

    startupText = code
    for body in chunkBodies.values():
        if body:
            startupText = startupText.replace(body, "")
    for name in referencesIn(startupText):
        if name in chunkBodies:
            worklist.append(name)

    while worklist:
        name = worklist.pop()
        if name in referenced or name not in chunkBodies:
            continue
        referenced.add(name)
        for target in referencesIn(chunkBodies[name]):
            if target in chunkBodies:
                worklist.append(target)

    for name, body in chunkBodies.items():
        if name not in referenced:
            code = code.replace(body, "")
    return code


############################### PARSER ###############################

typeSpecifierTokens = {"void", "char", "short", "int", "long", "float",
                       "double", "signed", "unsigned", "struct", "union",
                       "enum"}
storageClassTokens = {"typedef", "extern", "static", "auto", "register"}
qualifierTokens = {"const", "volatile"}


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens        # list ending with None
        self.pos = 0
        self.nakedFlag = False

    def peek(self, ahead=0):
        return self.tokens[self.pos + ahead]

    def peekType(self, ahead=0):
        token = self.tokens[self.pos + ahead]
        return token.type if token is not None else None

    def next(self):
        token = self.tokens[self.pos]
        if token is None:
            fail("unexpected end of input")
        self.pos += 1
        return token

    def accept(self, type):
        if self.peek() is not None and self.peek().type == type:
            return self.next()
        return None

    def expect(self, type, context):
        token = self.peek()
        if token is None or token.type != type:
            fail("expected '%s' %s but found '%s'"
                 % (type, context, token.name if token else "end of input"),
                 token.line if token else None)
        return self.next()

    def line(self):
        token = self.peek()
        return token.line if token else None

    def mark(self, node):
        node.line = self.line()
        return node

    # ---------------------------------------------------------- types

    def startsDeclaration(self):
        token = self.peek()
        if token is None:
            return False
        if token.type in typeSpecifierTokens or token.type in storageClassTokens \
                or token.type in qualifierTokens:
            return True
        return token.type == "IDENTIFIER" and token.name in typedefNames

    def parseDeclarationSpecifiers(self):
        storageClass = None
        baseType = None
        intParts = []
        qualifiers = []
        self.nakedFlag = False
        line = self.line()
        while True:
            token = self.peek()
            if token is None:
                break
            if token.type == "__naked":
                self.nakedFlag = True
                self.next()
            elif token.type in storageClassTokens:
                if storageClass is not None:
                    fail("multiple storage classes", token.line)
                storageClass = token.type
                self.next()
            elif token.type in qualifierTokens:
                qualifiers.append(token.type)
                self.next()
            elif token.type in ("struct", "union"):
                baseType = self.parseStructSpecifier()
            elif token.type == "enum":
                baseType = self.parseEnumSpecifier()
            elif token.type == "float":
                baseType = FLOAT
                self.next()
            elif token.type == "double":
                fail("double precision floating point is not yet supported", token.line)
            elif token.type in ("void", "char", "short", "int", "long",
                                "signed", "unsigned"):
                intParts.append(token.type)
                self.next()
            elif token.type == "IDENTIFIER" and token.name in typedefNames \
                    and baseType is None and not intParts:
                baseType = typedefNames[token.name]
                self.next()
            else:
                break

        if baseType is None:
            if not intParts:
                fail("expected a type specifier", line)
            if "void" in intParts:
                baseType = VOID
            elif "long" in intParts:
                baseType = LongType(isUnsigned=("unsigned" in intParts))
            else:
                name = " ".join(intParts) if intParts else "int"
                baseType = IntType(name, isUnsigned=("unsigned" in intParts))
        # Apply qualifiers
        if baseType is not None and "const" in qualifiers:
            baseType.isConst = True
        if baseType is not None and "volatile" in qualifiers:
            baseType.isVolatile = True
        return storageClass, baseType

    def parseStructSpecifier(self):
        keyword = self.next()       # struct / union
        isUnion = keyword.type == "union"
        tag = None
        if self.peek().type == "IDENTIFIER":
            tag = self.next().name

        structType = None
        if tag is not None:
            key = ("union " if isUnion else "struct ") + tag
            structType = structTags.get(key)

        if self.peek().type == "{":
            if structType is None:
                structType = StructType(tag, isUnion)
                if tag is not None:
                    structTags[("union " if isUnion else "struct ") + tag] = structType
            elif structType.members is not None:
                fail("redefinition of '%s'" % structType, keyword.line)
            self.next()
            members = []
            while self.peek().type != "}":
                _, memberBase = self.parseDeclarationSpecifiers()
                while True:
                    name, ctype = self.parseDeclarator(memberBase)
                    if name is None:
                        fail("unnamed struct member", keyword.line)
                    if self.accept(":"):
                        fail("bit fields are not supported", keyword.line)
                    members.append((name, ctype))
                    if not self.accept(","):
                        break
                self.expect(";", "after struct member")
            self.next()
            structType.define(members)
        elif structType is None:
            if tag is None:
                fail("anonymous struct without member list", keyword.line)
            structType = StructType(tag, isUnion)
            structTags[("union " if isUnion else "struct ") + tag] = structType
        return structType

    def parseEnumSpecifier(self):
        keyword = self.next()
        if self.peek().type == "IDENTIFIER":
            self.next()             # tag: accepted and ignored (all enums are int)
        if self.peek().type == "{":
            self.next()
            value = 0
            while self.peek().type != "}":
                name = self.expect("IDENTIFIER", "in enum").name
                if self.accept("="):
                    value = evalConstSigned(self.parseConditionalExpression(),
                                            "enum value")
                if name in enumConstants:
                    fail("redefinition of enum constant '%s'" % name, keyword.line)
                enumConstants[name] = value & 0xffff
                value += 1
                if not self.accept(","):
                    break
            self.expect("}", "after enum")
        return IntType("int")

    def parseDeclarator(self, baseType, abstract=False):
        """returns (name or None, type)"""
        pointerCount = 0
        while self.peekType() == "*":
            self.next()
            pointerCount += 1
            while self.peekType() in qualifierTokens:
                self.next()

        # inner (parenthesized) declarator or identifier
        innerTokensStart = None
        name = None
        if self.peekType() == "(" and self.isDeclaratorParen():
            self.next()
            innerTokensStart = self.pos
            depth = 1
            while depth > 0:
                token = self.next()
                if token.type == "(":
                    depth += 1
                elif token.type == ")":
                    depth -= 1
            innerTokensEnd = self.pos - 1
        elif self.peekType() == "IDENTIFIER":
            name = self.next().name
        elif not abstract:
            fail("expected identifier in declarator", self.line())

        # suffixes
        suffixes = []
        while True:
            if self.peekType() == "[":
                self.next()
                if self.peekType() == "]":
                    length = None
                else:
                    length = evalConstSigned(self.parseConditionalExpression(),
                                             "array size")
                    if length <= 0:
                        fail("array size must be positive", self.line())
                self.expect("]", "after array size")
                suffixes.append(("array", length))
            elif self.peekType() == "(":
                self.next()
                params, ellipsis = self.parseParameterList()
                self.expect(")", "after parameter list")
                suffixes.append(("function", params, ellipsis))
            else:
                break

        # pointers bind to the base type, suffixes to the declarator:
        # char *f(int) is a function returning char*, int *a[3] an array
        # of pointers, and int (*f)(int) resolves via the inner declarator
        ctype = baseType
        for _ in range(pointerCount):
            ctype = PointerType(ctype)
        for suffix in reversed(suffixes):
            if suffix[0] == "array":
                if ctype.isFunction():
                    fail("array of functions is not allowed", self.line())
                ctype = ArrayType(ctype, suffix[1])
            else:
                if ctype.isArray():
                    fail("function returning array is not allowed", self.line())
                ctype = FunctionType(ctype, suffix[1], suffix[2])

        if innerTokensStart is not None:
            innerParser = Parser(self.tokens[innerTokensStart:innerTokensEnd] + [None])
            name, ctype = innerParser.parseDeclarator(ctype, abstract)
        return name, ctype

    def isDeclaratorParen(self):
        """distinguish  (*f)(...)  from a function parameter list  (int a)"""
        token = self.peek(1)
        if token is None:
            return False
        if token.type in ("*", "("):
            return True
        return token.type == "IDENTIFIER" and token.name not in typedefNames

    def parseParameterList(self):
        """returns (params or None, ellipsis); params: list of (name, type)"""
        if self.peek().type == ")":
            return None, False              # unprototyped ()
        if self.peek().type == "void" and self.peek(1).type == ")":
            self.next()
            return [], False
        params = []
        ellipsis = False
        while True:
            if self.peek().type == "...":
                self.next()
                ellipsis = True
                break
            _, base = self.parseDeclarationSpecifiers()
            name, ctype = self.parseDeclarator(base, abstract=True)
            if ctype.isArray():
                ctype = PointerType(ctype.element)      # arrays decay
            if ctype.isFunction():
                ctype = PointerType(ctype)
            if ctype.kind == "void":
                fail("parameter has void type", self.line())
            params.append((name, ctype))
            if not self.accept(","):
                break
        return params, ellipsis

    def parseTypeName(self):
        _, base = self.parseDeclarationSpecifiers()
        name, ctype = self.parseDeclarator(base, abstract=True)
        if name is not None:
            fail("unexpected identifier in type name", self.line())
        return ctype

    def startsTypeName(self):
        token = self.peek()
        if token is None:
            return False
        if token.type in typeSpecifierTokens or token.type in qualifierTokens:
            return True
        return token.type == "IDENTIFIER" and token.name in typedefNames

    # ----------------------------------------------------- expressions

    def parsePrimaryExpression(self):
        token = self.next()
        if token.type == "IDENTIFIER":
            node = VariableAccess(token.name)
        elif token.type == "CONSTANT":
            node = Constant(token.value)
        elif token.type == "FLOAT":
            node = FloatConstant(token.value)
        elif token.type == "STRING":
            text = token.value
            while self.peek() is not None and self.peek().type == "STRING":
                text += self.next().value       # adjacent string concatenation
            node = StringLiteral(text)
        elif token.type == "(":
            node = self.parseExpression()
            self.expect(")", "after parenthesized expression")
        else:
            fail("unexpected token '%s' in expression" % token.name, token.line)
        node.line = token.line
        return node

    def parsePostfixExpression(self):
        node = self.parsePrimaryExpression()
        while True:
            token = self.peek()
            if token is None:
                break
            if token.type == "[":
                self.next()
                index = self.parseExpression()
                self.expect("]", "after array index")
                node = self.mark(ArrayAccess(node, index))
            elif token.type == "(":
                self.next()
                arguments = []
                if self.peek().type != ")":
                    while True:
                        arguments.append(self.parseAssignmentExpression())
                        if not self.accept(","):
                            break
                self.expect(")", "after function arguments")
                node = self.mark(FunctionCall(node, arguments))
            elif token.type == ".":
                self.next()
                member = self.expect("IDENTIFIER", "after '.'").name
                node = self.mark(MemberAccess(node, member, arrow=False))
            elif token.type == "->":
                self.next()
                member = self.expect("IDENTIFIER", "after '->'").name
                node = self.mark(MemberAccess(node, member, arrow=True))
            elif token.type == "++":
                self.next()
                node = self.mark(PostIncDec(node, "++"))
            elif token.type == "--":
                self.next()
                node = self.mark(PostIncDec(node, "--"))
            else:
                break
        return node

    def parseUnaryExpression(self):
        token = self.peek()
        if token.type == "++":
            self.next()
            return self.mark(PreIncDec(self.parseUnaryExpression(), "++"))
        if token.type == "--":
            self.next()
            return self.mark(PreIncDec(self.parseUnaryExpression(), "--"))
        if token.type in UnaryOperation.operators:
            self.next()
            operand = self.parseCastExpression()
            if token.type == "-" and isinstance(operand, FloatConstant):
                # fold -1.5 into a constant so initializers stay constant
                return self.mark(FloatConstant(operand.bits ^ 0x80000000))
            return self.mark(UnaryOperation(token.type, operand))
        if token.type == "sizeof":
            self.next()
            if self.peek().type == "(" and self.startsTypeNameAfterParen():
                self.next()
                typeName = self.parseTypeName()
                self.expect(")", "after sizeof type")
                return self.mark(SizeofType(typeName))
            return self.mark(SizeofExpression(self.parseUnaryExpression()))
        return self.parsePostfixExpression()

    def startsTypeNameAfterParen(self):
        saved = self.pos
        self.pos += 1
        result = self.startsTypeName()
        self.pos = saved
        return result

    def parseCastExpression(self):
        if self.peek().type == "(" and self.startsTypeNameAfterParen():
            line = self.line()
            self.next()
            typeName = self.parseTypeName()
            self.expect(")", "after type cast")
            node = Typecast(typeName, self.parseCastExpression())
            node.line = line
            return node
        return self.parseUnaryExpression()

    def parseBinaryExpression(self, level=0):
        levels = [["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="],
                  ["<", ">", "<=", ">="], ["<<", ">>"], ["+", "-"],
                  ["*", "/", "%"]]
        if level == len(levels):
            return self.parseCastExpression()
        node = self.parseBinaryExpression(level + 1)
        while self.peek() is not None and self.peek().type in levels[level]:
            operator = self.next().type
            right = self.parseBinaryExpression(level + 1)
            node = self.mark(BinaryOperator(operator, node, right))
        return node

    def parseConditionalExpression(self):
        node = self.parseBinaryExpression()
        if self.peek() is not None and self.peek().type == "?":
            self.next()
            trueExpression = self.parseExpression()
            self.expect(":", "in conditional expression")
            falseExpression = self.parseConditionalExpression()
            node = self.mark(ConditionalExpression(node, trueExpression,
                                                   falseExpression))
        return node

    def parseAssignmentExpression(self):
        node = self.parseConditionalExpression()
        token = self.peek()
        if token is not None and (token.type == "=" or token.type in assignmentOperators):
            self.next()
            value = self.parseAssignmentExpression()
            node = self.mark(Assignment(token.type, node, value))
        return node

    def parseExpression(self):
        expressions = [self.parseAssignmentExpression()]
        while self.accept(","):
            expressions.append(self.parseAssignmentExpression())
        if len(expressions) == 1:
            return expressions[0]
        return self.mark(Expression(expressions))

    # ------------------------------------------------------ statements

    def parseStatement(self):
        token = self.peek()
        line = token.line

        if token.type == "{":
            return self.parseCompoundStatement()
        if token.type == ";":
            self.next()
            return ExpressionStatement(None)
        if token.type == "if":
            self.next()
            self.expect("(", "after 'if'")
            condition = self.parseExpression()
            self.expect(")", "after if condition")
            thenStatement = self.parseStatement()
            elseStatement = None
            if self.accept("else"):
                elseStatement = self.parseStatement()
            node = ConditionalStatement(condition, thenStatement, elseStatement)
        elif token.type == "switch":
            self.next()
            self.expect("(", "after 'switch'")
            expression = self.parseExpression()
            self.expect(")", "after switch expression")
            node = SwitchStatement(expression, self.parseStatement())
        elif token.type == "while":
            self.next()
            self.expect("(", "after 'while'")
            expression = self.parseExpression()
            self.expect(")", "after while condition")
            node = WhileLoop(expression, self.parseStatement())
        elif token.type == "do":
            self.next()
            statement = self.parseStatement()
            self.expect("while", "after do body")
            self.expect("(", "after 'while'")
            expression = self.parseExpression()
            self.expect(")", "after do-while condition")
            self.expect(";", "after do-while")
            node = DoWhileLoop(statement, expression)
        elif token.type == "for":
            self.next()
            self.expect("(", "after 'for'")
            initial = None if self.peek().type == ";" else self.parseExpression()
            self.expect(";", "in for statement")
            condition = None if self.peek().type == ";" else self.parseExpression()
            self.expect(";", "in for statement")
            post = None if self.peek().type == ")" else self.parseExpression()
            self.expect(")", "after for header")
            node = ForLoop(initial, condition, post, self.parseStatement())
        elif token.type == "goto":
            self.next()
            name = self.expect("IDENTIFIER", "after 'goto'").name
            self.expect(";", "after goto")
            node = Goto(name)
        elif token.type == "continue":
            self.next()
            self.expect(";", "after 'continue'")
            node = Continue()
        elif token.type == "break":
            self.next()
            self.expect(";", "after 'break'")
            node = Break()
        elif token.type == "return":
            self.next()
            expression = None if self.peek().type == ";" else self.parseExpression()
            self.expect(";", "after return")
            node = Return(expression)
        elif token.type == "case":
            self.next()
            value = self.parseConditionalExpression()
            self.expect(":", "after case value")
            node = CaseLabel(value, self.parseStatement())
        elif token.type == "default":
            self.next()
            self.expect(":", "after 'default'")
            node = CaseLabel(None, self.parseStatement())
        elif token.type == "__asm__":
            self.next()
            self.expect("(", "after __asm__")
            text = ""
            while self.peek().type == "STRING":
                text += self.next().value
            self.expect(")", "after __asm__ string")
            self.expect(";", "after __asm__ statement")
            node = InlineAssembler(text)
        elif token.type == "IDENTIFIER" and self.peek(1) is not None \
                and self.peek(1).type == ":":
            name = self.next().name
            self.next()
            node = GotoLabel(name, self.parseStatement())
        else:
            expression = self.parseExpression()
            self.expect(";", "after expression")
            node = ExpressionStatement(expression)
        node.line = line
        return node

    def parseCompoundStatement(self):
        self.expect("{", "at start of block")
        declarations = []
        while self.startsDeclaration():
            declarations.append(self.parseLocalDeclaration())
        statements = []
        while self.peek() is not None and self.peek().type != "}":
            statements.append(self.parseStatement())
        self.expect("}", "at end of block")
        return CompoundStatement(declarations, statements)

    def parseLocalDeclaration(self):
        line = self.line()
        storageClass, baseType = self.parseDeclarationSpecifiers()
        declarators = []
        if self.peek().type != ";":
            while True:
                name, ctype = self.parseDeclarator(baseType)
                initializer = None
                if self.accept("="):
                    initializer = self.parseInitializer()
                if storageClass == "typedef":
                    typedefNames[name] = ctype
                else:
                    declarators.append((name, ctype, initializer))
                if not self.accept(","):
                    break
        self.expect(";", "after declaration")
        node = Declaration(storageClass, declarators)
        node.line = line
        return node

    def parseInitializer(self):
        if self.peek().type == "{":
            self.next()
            items = []
            while self.peek().type != "}":
                items.append(self.parseInitializer())
                if not self.accept(","):
                    break
            self.expect("}", "after initializer list")
            return items
        return self.parseAssignmentExpression()

    # ------------------------------------------------ external decls

    def parseProgram(self):
        externalDeclarations = []
        while self.peek() is not None:
            externalDeclarations.append(self.parseExternalDeclaration())
        return Program(externalDeclarations)

    def parseExternalDeclaration(self):
        line = self.line()
        storageClass, baseType = self.parseDeclarationSpecifiers()

        if self.peek() is not None and self.peek().type == ";":
            self.next()             # bare struct/enum declaration
            node = GlobalDeclaration(storageClass, [])
            node.line = line
            return node

        name, ctype = self.parseDeclarator(baseType)

        if self.peek() is not None and self.peek().type == "{":
            if not ctype.isFunction():
                fail("expected ';' after declaration", self.line())
            if storageClass == "typedef":
                fail("typedef with function body", self.line())
            parameterNames = [p[0] for p in ctype.params or []]
            body = self.parseCompoundStatement()
            node = FunctionDefinition(name, ctype, parameterNames, body,
                                      naked=self.nakedFlag)
            node.line = line
            return node

        declarators = []
        while True:
            initializer = None
            if self.accept("="):
                initializer = self.parseInitializer()
            if storageClass == "typedef":
                typedefNames[name] = ctype
            else:
                declarators.append((name, ctype, initializer))
            if not self.accept(","):
                break
            name, ctype = self.parseDeclarator(baseType)
        self.expect(";", "after declaration")
        node = GlobalDeclaration(storageClass, declarators)
        node.line = line
        return node


############################### AST DUMP ###############################

def dumpAst(node, indent=0):
    pad = "  " * indent
    if isinstance(node, list):
        for item in node:
            dumpAst(item, indent)
        return
    if not isinstance(node, Node):
        print(pad + repr(node))
        return
    fields = {k: v for k, v in vars(node).items()
              if k not in ("line", "ctype", "switchLabel") and v is not None}
    scalars = {k: v for k, v in fields.items()
               if not isinstance(v, (Node, list))}
    print(pad + type(node).__name__ + " " +
          " ".join("%s=%r" % kv for kv in scalars.items()))
    for key, value in fields.items():
        if isinstance(value, (Node, list)) and value != []:
            print(pad + "  ." + key + ":")
            dumpAst(value, indent + 2)


############################### PEEPHOLE OPTIMIZER ###############################
#
# Works on the generated assembly text after code generation and dead
# function elimination.  All rules are local rewrites on small windows
# of instructions; windows never extend across labels, so code reached
# by a jump is never mixed into a window.  Register liveness questions
# ("is r1 still needed after this instruction?") are answered by a
# conservative forward scan that follows direct jumps and gives up (=
# assumes live) at anything it cannot see through.

_TERNARY_OPS = {"add", "sub", "lmul", "hmul", "div", "mod", "shl", "shr",
                "and", "or", "xor"}
_IMM_ALU_OPS = {"addi", "subi", "lmuli", "hmuli", "divi", "modi", "shli",
                "shri", "andi", "ori", "xori"}
_OPS32 = {"add32", "sub32", "lmul32", "hmul32", "div32", "mod32",
          "shl32", "shr32", "and32", "or32", "xor32"}
_JUMP_OPS = {"jmp", "jmz", "jnz", "jlt", "jnlt", "jeq", "jneq", "jgt",
             "jngt", "jms", "jns", "jmc", "jnc", "jmb", "jnb", "jkb"}
_RJUMP_OPS = {j + "r" for j in _JUMP_OPS}
_ER_PAIRS = {"er%d" % i: ("r%d" % (2 * i), "r%d" % (2 * i + 1))
             for i in range(8)}


def _operandRegs(operand):
    """set of registers named by an operand ('F' = ALU/compare flags)"""
    name = operand.lstrip("*#").lower()
    if name in _ER_PAIRS:
        return set(_ER_PAIRS[name])
    if re.fullmatch(r"r1[0-5]|r[0-9]", name):
        return {name}
    if name in ("sp", "page", "pc"):
        return {name}
    if name == "state":
        return {"F"}
    return set()


class AsmInstruction:
    """One parsed line of generated assembly with its register effects."""

    def __init__(self, text):
        self.text = text
        self.label = None           # label name if this line defines one
        self.op = ""
        self.args = []
        self.reads = set()
        self.writes = set()
        self.flow = None            # None|'jump'|'branch'|'rjump'|'call'|'ret'
        self.target = None          # label of a jump/branch
        self.stack = False          # pushes, pops or rewrites sp
        self.known = True           # False: treat as a full barrier

        stripped = text.split(";")[0].strip()
        if stripped == "":
            return
        if stripped.endswith(":") and " " not in stripped:
            self.label = stripped[:-1]
            return
        parts = [p for p in re.split(r"[ \t]+|,\s*", stripped) if p]
        self.op = parts[0].lower()
        self.args = parts[1:]
        try:
            self._classify()
        except IndexError:
            self.known = False

    def _classify(self):
        op, A = self.op, self.args
        if op == "nop":
            pass
        elif op == "in":
            self.writes |= _operandRegs(A[0])
        elif op == "mov":
            dst, src = A[0].lower(), A[1].lower()
            if dst in ("sp", "page"):
                self.reads |= _operandRegs(A[1])
                self.writes.add(dst)
                self.stack = dst == "sp"
            else:
                self.reads |= _operandRegs(A[1])
                self.writes |= _operandRegs(A[0])
                if src == "sp":
                    self.stack = True   # conservative: reads sp
        elif op == "mov32":
            self.reads |= _operandRegs(A[1])
            self.writes |= _operandRegs(A[0])
        elif op in _TERNARY_OPS:
            if len(A) == 3:
                self.reads |= _operandRegs(A[1]) | _operandRegs(A[2])
                self.writes |= _operandRegs(A[0]) | {"F"}
            else:                       # auto-immediate or er-pair form
                self.reads |= _operandRegs(A[0]) | _operandRegs(A[1])
                self.writes |= _operandRegs(A[0]) | {"F"}
        elif op in _IMM_ALU_OPS:
            self.reads |= _operandRegs(A[0])
            self.writes |= _operandRegs(A[0]) | {"F"}
        elif op in ("negi", "noti"):
            self.writes |= _operandRegs(A[0]) | {"F"}
        elif op in ("neg", "not"):
            self.reads |= _operandRegs(A[1])
            self.writes |= _operandRegs(A[0]) | {"F"}
        elif op in _OPS32:
            self.reads |= _operandRegs(A[0]) | _operandRegs(A[1])
            self.writes |= _operandRegs(A[0]) | {"F"}
        elif op in ("neg32", "not32"):
            self.reads |= _operandRegs(A[1])
            self.writes |= _operandRegs(A[0]) | {"F"}
        elif op in ("cmp", "cmpi", "cmp32"):
            for a in A:
                self.reads |= _operandRegs(a)
            self.writes.add("F")
        elif op in ("ldr", "ldo"):
            self.reads |= _operandRegs(A[1])
            self.writes |= _operandRegs(A[0])
        elif op == "ldd":
            self.writes |= _operandRegs(A[0])
        elif op == "stor":
            self.reads |= _operandRegs(A[0]) | _operandRegs(A[1])
        elif op == "stoo":
            self.reads |= _operandRegs(A[0]) | _operandRegs(A[2])
        elif op == "stod":
            self.reads |= _operandRegs(A[1])
        elif op == "stoi":
            self.reads |= _operandRegs(A[0])
        elif op in ("push", "pushi"):
            if A:
                self.reads |= _operandRegs(A[0])
            self.stack = True
        elif op in ("pop", "popi"):
            if A:
                self.writes |= _operandRegs(A[0])
            self.stack = True
        elif op == "ldpc":
            self.writes |= _operandRegs(A[0])
        elif op == "ldsp":
            self.reads.add("sp")
            self.writes |= _operandRegs(A[0])
        elif op == "stosp":
            self.reads |= _operandRegs(A[0])
            self.writes.add("sp")
            self.stack = True
        elif op == "ldpage":
            self.reads.add("page")
            self.writes |= _operandRegs(A[0])
        elif op == "stopage":
            self.reads |= _operandRegs(A[0])
            self.writes.add("page")
        elif op == "ldstate":
            self.reads.add("F")
            self.writes |= _operandRegs(A[0])
        elif op in _JUMP_OPS:
            regs = _operandRegs(A[0])
            if regs:                    # register operand: indirect jump
                self.reads |= regs | {"F"}
                self.flow = "rjump"
            elif op == "jmp":
                self.flow = "jump"
                self.target = A[0].lstrip("*#")
            else:
                self.reads.add("F")
                self.flow = "branch"
                self.target = A[0].lstrip("*#")
        elif op in _RJUMP_OPS:
            self.reads |= _operandRegs(A[0]) | {"F"}
            self.flow = "rjump"
        elif op == "call":              # expands to ldpc r0/addi/push/jmp
            self.writes |= {"r0", "F"}
            self.stack = True
            self.flow = "call"
            self.target = A[0]
        elif op == "callr":             # expands to mov r1/ldpc r0/push/jmpr
            self.reads |= _operandRegs(A[0])
            self.writes |= {"r0", "r1", "F"}
            self.stack = True
            self.flow = "call"
        elif op == "ret":               # expands to pop r1/jmpr r1
            self.writes.add("r1")
            self.stack = True
            self.flow = "ret"
        else:
            self.known = False


def _immValue(operand):
    """numeric value of an immediate operand, or None"""
    text = operand.lstrip("*#")
    try:
        if text.lower().startswith("0x") or text.lower().startswith("-0x"):
            return int(text, 16)
        return int(text)
    except ValueError:
        return None


def _regsDeadFrom(instrs, labels, start, regs, depth=0, visited=None):
    """True if, starting at instrs[start], every register in `regs` is
    (on every path) overwritten before it is read.  Conservative: gives
    up on anything it cannot follow."""
    if depth > 6:
        return False
    if visited is None:
        visited = set()
    live = frozenset(regs)
    i = start
    steps = 0
    while i < len(instrs):
        key = (i, live)
        if key in visited:
            return True             # revisited state: no read on this loop
        visited.add(key)
        steps += 1
        if steps > 1000:
            return False
        ins = instrs[i]
        if ins.label is not None or ins.op in ("", "nop"):
            i += 1
            continue
        if not ins.known:
            return False
        if ins.reads & live:
            return False
        live = live - frozenset(ins.writes)
        if not live:
            return True
        if ins.flow is None:
            i += 1
            continue
        if ins.flow == "jump":
            if ins.target not in labels:
                return False
            i = labels[ins.target]
            continue
        if ins.flow == "branch":
            if ins.target not in labels:
                return False
            if not _regsDeadFrom(instrs, labels, labels[ins.target], live,
                                 depth + 1, visited):
                return False
            i += 1
            continue
        if ins.flow == "call" and ins.target is not None \
                and ins.target in labels:
            # scan into the callee's actual code; its 'ret' (which
            # writes r1) ends the scan there, so anything still live
            # at that point stays conservatively live
            i = labels[ins.target]
            continue
        return False                # callr/ret/rjump: cannot see further
    return False


def _nextIdx(instrs, i):
    """index of the next instruction line, or None at a label/barrier/end"""
    j = i + 1
    while j < len(instrs):
        ins = instrs[j]
        if ins.label is not None:
            return None
        if ins.op == "":
            j += 1
            continue
        return j
    return None


def _window(instrs, i, count):
    """indices of `count` consecutive instructions starting at i, not
    crossing labels; None if unavailable"""
    result = [i]
    j = i
    while len(result) < count:
        j = _nextIdx(instrs, j)
        if j is None:
            return None
        result.append(j)
    return result


def _isCallIdiomJump(instrs, i):
    """True if instrs[i] is the jmp of a ldpc/addi/push/jmp call sequence
    (the following instruction is a return address, not dead code)"""
    seen = 0
    j = i - 1
    while j >= 0 and seen < 3:
        ins = instrs[j]
        if ins.label is not None:
            return False
        if ins.op != "":
            if ins.op == "ldpc":
                return True
            seen += 1
        j -= 1
    return False


def _rulePushPop(instrs, labels, i):
    """push rA / <block not touching rB or the stack> / pop rB
       →  mov rB,rA / <block>        (dropped entirely if rA == rB)"""
    ins = instrs[i]
    if ins.op != "push" or not ins.args:
        return None
    srcRegs = _operandRegs(ins.args[0])
    if len(srcRegs) != 1:
        return None
    rA = next(iter(srcRegs))
    if rA in ("sp", "page", "pc", "F"):
        return None
    block = []
    j = i
    while len(block) <= 8:
        j = _nextIdx(instrs, j)
        if j is None:
            return None
        b = instrs[j]
        if b.op == "pop" and b.args:
            dstRegs = _operandRegs(b.args[0])
            if len(dstRegs) != 1:
                return None
            rB = next(iter(dstRegs))
            if rB in ("sp", "page", "pc", "F"):
                return None
            if rA == rB:
                for blk in block:
                    if rA in blk.writes:
                        return None
                return ([blk.text for blk in block], j - i + 1)
            for blk in block:
                if rB in blk.reads or rB in blk.writes:
                    return None
            return (["\tmov %s,%s" % (rB, rA)] + [blk.text for blk in block],
                    j - i + 1)
        if (not b.known or b.flow is not None or b.stack or b.op == "ldpc"
                or "sp" in b.reads or "sp" in b.writes or "page" in b.writes):
            return None
        block.append(b)
    return None


def _ruleStoreLoad(instrs, labels, i):
    """a store immediately followed by a load of the same location:
    the value is still in the source register"""
    a = instrs[i]
    j = _nextIdx(instrs, i)
    if j is None:
        return None
    b = instrs[j]
    match = None                    # (stored reg, loaded reg)
    if a.op == "stoo" and b.op == "ldo" and len(a.args) == 3 \
            and len(b.args) == 3 \
            and a.args[0].lstrip("*").lower() == b.args[1].lstrip("*").lower() \
            and _immValue(a.args[1]) is not None \
            and (_immValue(a.args[1]) & 0xffff) == ((_immValue(b.args[2]) or 0) & 0xffff) \
            and _immValue(b.args[2]) is not None:
        match = (a.args[2].lower(), b.args[0].lower())
    elif a.op == "stod" and b.op == "ldd" and len(a.args) == 2 \
            and len(b.args) == 2 \
            and _immValue(a.args[0]) is not None \
            and (_immValue(a.args[0]) & 0xffff) == ((_immValue(b.args[1]) or 0) & 0xffff) \
            and _immValue(b.args[1]) is not None:
        match = (a.args[1].lower(), b.args[0].lower())
    elif a.op == "stor" and b.op == "ldr" and len(a.args) == 2 \
            and len(b.args) == 2 \
            and a.args[0].lstrip("*").lower() == b.args[1].lstrip("*").lower():
        match = (a.args[1].lower(), b.args[0].lower())
    if match is None:
        return None
    stored, loaded = match
    if len(_operandRegs(stored)) != 1 or len(_operandRegs(loaded)) != 1:
        return None
    if stored == loaded:
        return ([a.text], j - i + 1)
    return ([a.text, "\tmov %s,%s" % (loaded, stored)], j - i + 1)


_FOLD_IMM = {
    "addi": lambda a, b: a + b,
    "subi": lambda a, b: a - b,
    "lmuli": lambda a, b: a * b,
    "andi": lambda a, b: a & b,
    "ori": lambda a, b: a | b,
    "xori": lambda a, b: a ^ b,
    "shli": lambda a, b: a << (b & 0x1f),
    "shri": lambda a, b: (a & 0xffff) >> (b & 0x1f),
}


def _ruleInFold(instrs, labels, i):
    """in rX,#a / opi rX,#b  →  in rX,#(a op b)   (flags must be dead)"""
    a = instrs[i]
    if a.op != "in" or len(a.args) != 2:
        return None
    j = _nextIdx(instrs, i)
    if j is None:
        return None
    b = instrs[j]
    if b.op not in _FOLD_IMM or len(b.args) != 2:
        return None
    if a.args[0].lower() != b.args[0].lower():
        return None
    va, vb = _immValue(a.args[1]), _immValue(b.args[1])
    if va is None or vb is None:
        return None
    if not _regsDeadFrom(instrs, labels, j + 1, {"F"}):
        return None
    folded = _FOLD_IMM[b.op](va & 0xffff, vb & 0xffff) & 0xffff
    return (["\tin %s,#%d" % (a.args[0].lower(), folded)], j - i + 1)


def _ruleImmOp(instrs, labels, i):
    """mov r1,r0 / in r0,#imm / op r0,r1,r0  →  opi r0,#imm
       mov r1,r0 / in r0,#imm / cmp r1,r0   →  cmp r0,#imm
    (r1 must be dead afterwards)"""
    w = _window(instrs, i, 3)
    if w is None:
        return None
    m1, m2, m3 = (instrs[k] for k in w)
    if m1.op != "mov" or [a.lower() for a in m1.args] != ["r1", "r0"]:
        return None
    if m2.op != "in" or len(m2.args) != 2 or m2.args[0].lower() != "r0":
        return None
    imm = m2.args[1]
    consumed = w[2] - i + 1
    if m3.op in _TERNARY_OPS and [a.lower() for a in m3.args] == ["r0", "r1", "r0"]:
        if not _regsDeadFrom(instrs, labels, w[2] + 1, {"r1"}):
            return None
        return (["\t%si r0,%s" % (m3.op, imm)], consumed)
    if m3.op == "cmp" and [a.lower() for a in m3.args] == ["r1", "r0"]:
        if not _regsDeadFrom(instrs, labels, w[2] + 1, {"r1"}):
            return None
        return (["\tcmp r0,%s" % imm], consumed)
    return None


def _ruleOperandSwap(instrs, labels, i):
    """mov r1,r0 / <X writing r0> / op r0,r1,r0  →  <X writing r1> / op r0,r0,r1
    (r1 must be dead afterwards)"""
    w = _window(instrs, i, 3)
    if w is None:
        return None
    m1, m2, m3 = (instrs[k] for k in w)
    if m1.op != "mov" or [a.lower() for a in m1.args] != ["r1", "r0"]:
        return None
    if m2.op not in ("in", "ldo", "ldd", "ldr", "mov") or not m2.args:
        return None
    if m2.args[0].lower() != "r0" or m2.writes != {"r0"}:
        return None
    if {"r0", "r1"} & m2.reads:
        return None
    rewritten = "\t%s r1,%s" % (m2.op, ",".join(m2.args[1:]))
    consumed = w[2] - i + 1
    if m3.op in _TERNARY_OPS and [a.lower() for a in m3.args] == ["r0", "r1", "r0"]:
        if not _regsDeadFrom(instrs, labels, w[2] + 1, {"r1"}):
            return None
        return ([rewritten, "\t%s r0,r0,r1" % m3.op], consumed)
    if m3.op == "cmp" and [a.lower() for a in m3.args] == ["r1", "r0"]:
        if not _regsDeadFrom(instrs, labels, w[2] + 1, {"r1"}):
            return None
        return ([rewritten, "\tcmp r0,r1"], consumed)
    return None


# state register bit/shift → branch op when the boolean is (0 / not 0)
_FUSE_SIMPLE = {(16, 4): ("jneq", "jeq"), (32, 5): ("jeq", "jneq"),
                (4, 2): ("jnlt", "jlt"), (64, 6): ("jngt", "jgt")}
_FUSE_ORED = {(4, 2): ("jgt", "jngt"), (64, 6): ("jlt", "jnlt")}


def _matchArgs(ins, op, *args):
    return ins.op == op and [a.lower() for a in ins.args] == list(args)


def _ruleCmpFuse(instrs, labels, i):
    """Fuse the 'extract flag bit into r0, compare with 0, branch'
    idiom emitted for comparisons in conditions into a single
    conditional branch on the original compare flags."""
    if not _matchArgs(instrs[i], "mov", "r0", "state"):
        return None

    # <= / >= : the eq bit is OR-ed in via r1
    w = _window(instrs, i, 9)
    if w is not None:
        m = [instrs[k] for k in w]
        if (_matchArgs(m[1], "mov", "r1", "r0")
                and m[2].op == "andi" and m[2].args[0].lower() == "r0"
                and m[3].op == "shri" and m[3].args[0].lower() == "r0"
                and _matchArgs(m[4], "andi", "r1", "#16")
                and _matchArgs(m[5], "shri", "r1", "#4")
                and _matchArgs(m[6], "or", "r0", "r1", "r0")
                and _matchArgs(m[7], "cmp", "r0", "#0")
                and m[8].op in ("jeq", "jneq") and m[8].flow == "branch"
                and m[8].target in labels):
            key = (_immValue(m[2].args[1]), _immValue(m[3].args[1]))
            if key in _FUSE_ORED:
                branch = _FUSE_ORED[key][0 if m[8].op == "jeq" else 1]
                dead = {"r0", "r1", "F"}
                if _regsDeadFrom(instrs, labels, w[8] + 1, dead) and \
                        _regsDeadFrom(instrs, labels, labels[m[8].target], dead):
                    return (["\t%s %s" % (branch, m[8].target)], w[8] - i + 1)

    # ==, !=, <, > : single flag bit
    w = _window(instrs, i, 5)
    if w is None:
        return None
    m = [instrs[k] for k in w]
    if not (m[1].op == "andi" and m[1].args[0].lower() == "r0"
            and m[2].op == "shri" and m[2].args[0].lower() == "r0"
            and _matchArgs(m[3], "cmp", "r0", "#0")
            and m[4].op in ("jeq", "jneq") and m[4].flow == "branch"
            and m[4].target in labels):
        return None
    key = (_immValue(m[1].args[1]), _immValue(m[2].args[1]))
    if key not in _FUSE_SIMPLE:
        return None
    branch = _FUSE_SIMPLE[key][0 if m[4].op == "jeq" else 1]
    dead = {"r0", "F"}
    if not _regsDeadFrom(instrs, labels, w[4] + 1, dead):
        return None
    if not _regsDeadFrom(instrs, labels, labels[m[4].target], dead):
        return None
    return (["\t%s %s" % (branch, m[4].target)], w[4] - i + 1)


def _firstOpFrom(instrs, start):
    """index of the first instruction line at/after start, skipping
    labels and blanks"""
    j = start
    while j < len(instrs):
        if instrs[j].label is None and instrs[j].op != "":
            return j
        j += 1
    return None


def _ruleJumpThread(instrs, labels, i):
    """a jump to an unconditional jmp goes straight to the final target"""
    ins = instrs[i]
    if ins.flow not in ("jump", "branch") or ins.target not in labels:
        return None
    k = _firstOpFrom(instrs, labels[ins.target])
    if k is None or k == i:
        return None
    t = instrs[k]
    if t.flow != "jump" or t.op != "jmp" or t.target == ins.target:
        return None
    if t.target not in labels:
        return None
    # don't create a jump-to-self (the simulator's halt idiom)
    if _firstOpFrom(instrs, labels[t.target]) == i:
        return None
    return (["\t%s %s" % (ins.op, t.target)], 1)


def _ruleJumpToNext(instrs, labels, i):
    """a jump whose target label follows immediately does nothing"""
    ins = instrs[i]
    if ins.flow not in ("jump", "branch") or ins.target is None:
        return None
    if ins.op == "jmp" and _isCallIdiomJump(instrs, i):
        return None
    j = i + 1
    while j < len(instrs):
        b = instrs[j]
        if b.label is not None:
            if b.label == ins.target:
                return ([], 1)
            j += 1
            continue
        if b.op == "":
            j += 1
            continue
        return None
    return None


def _ruleUnreachable(instrs, labels, i):
    """instructions between an unconditional control transfer and the
    next label can never execute"""
    ins = instrs[i]
    if ins.op == "jmp" and ins.flow == "jump":
        if _isCallIdiomJump(instrs, i):
            return None
    elif ins.flow != "ret" and ins.op != "jmpr":
        return None
    j = i + 1
    removed = 0
    while j < len(instrs) and instrs[j].label is None:
        if instrs[j].op != "":
            removed += 1
        j += 1
    if removed == 0:
        return None
    return ([ins.text], j - i)


_PEEPHOLE_RULES = (_rulePushPop, _ruleStoreLoad, _ruleInFold, _ruleImmOp,
                   _ruleOperandSwap, _ruleCmpFuse, _ruleJumpThread,
                   _ruleJumpToNext, _ruleUnreachable)


def _peepholeSweep(lines):
    instrs = [AsmInstruction(text) for text in lines]
    labels = {}
    for idx, ins in enumerate(instrs):
        if ins.label is not None:
            labels[ins.label] = idx
    out = []
    changed = False
    i = 0
    while i < len(instrs):
        ins = instrs[i]
        if ins.label is None and ins.op == "" and ins.text.strip() == "":
            i += 1                  # drop blank lines
            continue
        if ins.op == "mov" and len(ins.args) == 2 \
                and ins.args[0].lower() == ins.args[1].lower() \
                and _operandRegs(ins.args[0]):
            i += 1                  # mov rX,rX does nothing
            changed = True
            continue
        replacement = None
        if ins.label is None and ins.known:
            for rule in _PEEPHOLE_RULES:
                replacement = rule(instrs, labels, i)
                if replacement is not None:
                    break
        if replacement is None:
            out.append(ins.text)
            i += 1
        else:
            newLines, consumed = replacement
            out.extend(newLines)
            i += consumed
            changed = True
    return out, changed


def peephole(asm):
    """Iterate the peephole rules to a fixed point."""
    lines = asm.split("\n")
    for _ in range(30):
        lines, changed = _peepholeSweep(lines)
        if not changed:
            break
    return "\n".join(lines)


############################### DRIVER ###############################


def compile(inputFileName, includeDirs, showAst=False, keepDead=False,
            optimize=True):
    global cg, currentSourceName
    cg = Codegen()
    enumConstants.clear()
    structTags.clear()
    typedefNames.clear()

    preprocessor = Preprocessor(includeDirs)
    preprocessor.process(inputFileName)
    currentSourceName = inputFileName

    tokens = preprocessor.output + [None]
    program = Parser(tokens).parseProgram()

    if showAst:
        dumpAst(program.externalDeclarations)

    code = program.generate()
    if not keepDead:
        code = eliminateDeadFunctions(code, program.functionChunks)
    if optimize:
        code = peephole(code)
    return code + "\n"


def main():
    args = sys.argv[1:]
    showAst = "--ast" in args
    keepDead = "--keep-dead" in args
    optimize = "--no-opt" not in args
    args = [a for a in args if a not in ("--ast", "--keep-dead", "--no-opt")]

    includeDirs = [os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")]
    outputFileName = None
    positional = []
    i = 0
    while i < len(args):
        if args[i] == "-o":
            outputFileName = args[i + 1]
            i += 2
        elif args[i] == "-I":
            includeDirs.insert(0, args[i + 1])
            i += 2
        elif args[i].startswith("-I"):
            includeDirs.insert(0, args[i][2:])
            i += 1
        else:
            positional.append(args[i])
            i += 1

    if len(positional) != 1:
        print("usage: risky_c.py input.c [-o output.asm] [-I includedir] [--ast] [--keep-dead] [--no-opt]")
        sys.exit(1)

    inputFileName = positional[0]
    if outputFileName is None:
        outputFileName = os.path.splitext(inputFileName)[0] + ".asm"

    code = compile(inputFileName, includeDirs, showAst, keepDead, optimize)

    with open(outputFileName, "w") as f:
        f.write(code)


if __name__ == "__main__":
    main()
