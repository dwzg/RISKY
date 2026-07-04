#!/usr/bin/env python3
"""Test runner for the RISKY C toolchain.

For every tests/*.c: compile -> assemble -> simulate, then compare the
terminal output with the matching *.expected file.  An optional *.input
file is preloaded into the page 0xb data ROM (stdin).

usage: run_tests.py [testname ...]
"""

import os
import subprocess
import sys

testDir = os.path.dirname(os.path.abspath(__file__))
v3Dir = os.path.dirname(testDir)
sys.path.insert(0, v3Dir)

import risky_sim
import risky_asm


def runTest(name):
    cFile = os.path.join(testDir, name + ".c")
    expectedFile = os.path.join(testDir, name + ".expected")
    inputFile = os.path.join(testDir, name + ".input")
    asmFile = os.path.join(testDir, name + ".asm")

    result = subprocess.run(
        [sys.executable, os.path.join(v3Dir, "risky_c.py"), cFile, "-o", asmFile],
        capture_output=True, text=True)
    if result.returncode != 0:
        return "COMPILE FAILED:\n" + result.stderr + result.stdout

    with open(asmFile) as f:
        try:
            program = risky_asm.assemble(f.read().splitlines())
        except SystemExit:
            return "ASSEMBLY FAILED"
    words = [int(w, 16) for w in program]

    inputData = ""
    if os.path.isfile(inputFile):
        with open(inputFile) as f:
            inputData = f.read()

    sim, reason = risky_sim.runProgram(words, inputData, maxCycles=3000000)
    output = "".join(sim.output)

    with open(expectedFile) as f:
        expected = f.read()

    if output != expected:
        return ("OUTPUT MISMATCH (%s)\n--- expected ---\n%s\n--- got ---\n%s\n"
                % (reason, expected, output))
    if "halted" not in reason:
        return "DID NOT HALT: " + reason
    os.remove(asmFile)          # keep the .asm only for failing tests
    return None


def main():
    if len(sys.argv) > 1:
        names = sys.argv[1:]
    else:
        names = sorted(f[:-2] for f in os.listdir(testDir)
                       if f.endswith(".c") and os.path.exists(
                           os.path.join(testDir, f[:-2] + ".expected")))

    failed = 0
    for name in names:
        error = runTest(name)
        if error is None:
            print("PASS  " + name)
        else:
            failed += 1
            print("FAIL  " + name)
            print("      " + error.replace("\n", "\n      "))

    print("\n%d/%d tests passed" % (len(names) - failed, len(names)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
