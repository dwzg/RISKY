#!/usr/bin/env pwsh
<#
.SYNOPSIS
RISKY C toolchain driver: compile + assemble (+ optionally simulate)

.DESCRIPTION
PowerShell equivalent of the bash cc script.
  .\cc.ps1 prog.c            -> prog.asm + prog.hex
  .\cc.ps1 -Run prog.c       -> also run prog.hex in the simulator
  .\cc.ps1 -Run prog.c -InFile input -> run with 'input' preloaded as stdin ROM
  .\cc.ps1 -Run prog.c -KeyFile keys -> preload keyboard buffer from 'keys'

The .hex file can be loaded into the instruction ROM in Logisim.
#>

param(
    [switch]$Run,
    [Parameter(Mandatory=$false, Position=0)]
    [string]$Source,
    [Parameter(Mandatory=$false)]
    [string]$InFile,
    [Parameter(Mandatory=$false)]
    [string]$KeyFile
)

$ErrorActionPreference = "Stop"
$dir = $PSScriptRoot

# Resolve python3 (try python3 first, then python)
$py = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $py) {
    $py = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $py) {
    Write-Error "Python not found (tried python3 and python)"
    exit 1
}
$py = $py.Source

if (-not $Source) {
    Write-Host "usage: cc.ps1 [-Run] prog.c [-InFile inputfile] [-KeyFile keyfile]"
    exit 1
}

$base = $Source -replace '\.c$', ''

# Compile C -> ASM
& $py "$dir\risky_c.py" $Source -o "$base.asm"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Assemble ASM -> HEX
& $py "$dir\risky_asm.py" "$base.asm" "$base.hex"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "built $base.hex"

# Optionally simulate
if ($Run) {
    $simArgs = @("$dir\risky_sim.py", "$base.hex")
    if ($InFile)  { $simArgs += "-i"; $simArgs += $InFile }
    if ($KeyFile) { $simArgs += "-k"; $simArgs += $KeyFile }
    & $py $simArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
