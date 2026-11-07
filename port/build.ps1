# Runs make inside the toolchain container.  Usage: .\build.ps1 [make args]
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $MakeArgs)
$image = 'ghcr.io/vita2hos/devcontainer/vita2hos:latest'
docker run --rm -v "${PSScriptRoot}:/work" -w /work $image make -j4 @MakeArgs
exit $LASTEXITCODE
