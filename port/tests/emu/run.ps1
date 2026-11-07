# Runs one exefs folder in the Ryujinx build kept in Sims3-NX.  Only reads that project.
# usage: run.ps1 <folder under port> [seconds]
param([string] $Target = 'build/canary/canary_npdm_exefs', [int] $Seconds = 40)
$port = (Resolve-Path "$PSScriptRoot\..\..").Path
$ryu = (Resolve-Path "$port\..\..\Sims3-NX\third_party\chansey").Path
$keys = (Resolve-Path "$port\..\..\prod.keys").Path
$out = Join-Path $port ('build\emu\' + (Split-Path $Target -Leaf))
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*" -Recurse -Force -ErrorAction SilentlyContinue
docker run --rm --name cod-emu -v "${port}:/work:ro" -v "${ryu}:/ryu:ro" -v "${keys}:/keys/prod.keys:ro" `
    -v "${out}:/out" sims3-emu:latest sh /work/tests/emu/run.sh $Target $Seconds
"docker exit=$LASTEXITCODE"
Get-Content "$out\stdout.txt" -ErrorAction SilentlyContinue | Select-Object -Last 45
