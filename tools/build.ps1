# Runs tools/build.py with the same arguments.
$ErrorActionPreference = 'Stop'
& python (Join-Path $PSScriptRoot 'build.py') @args
exit $LASTEXITCODE
