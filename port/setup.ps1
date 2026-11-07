# Builds the SD card folder for the port from your own APK and OBB, with Docker only.
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -Apk <your.apk> -Obb <your.obb> [-Store]
#
# The result is dist\cod_st\: copy its contents to sdmc:/switch/cod_st/ next to cod_st.nro.
# Everything is made from your files on your PC; nothing in it may be shared.
#
# Stages (each skips what is already done, so a stopped run can be started again):
#   1. extract the APK and OBB             -> ..\gamedata\
#   2. copy the containers and libraries   -> dist\sd\  (-Store: OBB uncompressed, ~1 GB more, faster loads)
#   3. copy the managed DLLs               -> dist\sd\data\app\managed\
#   4. compile them ahead of time on emulated ARM with the game's own Mono   (slow: hours)
#   5. link the compiled code into .dll.so
#   6. translate and compile the shaders   -> dist\sd\shaders.pak
#   7. gather what the console needs       -> dist\cod_st\
#
# Shaders Unity makes at runtime that the pak lacks are written to sdmc:/switch/cod_st/shader_miss/.
# Copy that folder to the PC and run:  setup.ps1 -ShadersOnly -Seen <folder>   then copy the new shaders.pak.
param(
    [string] $Apk,
    [string] $Obb,
    [switch] $Store,
    [switch] $ShadersOnly,
    [string] $Seen,
    [string] $GameData = (Join-Path $PSScriptRoot '..\gamedata'),
    [string] $Out = (Join-Path $PSScriptRoot 'dist\cod_st')
)
$ErrorActionPreference = 'Stop'
$image = 'ghcr.io/vita2hos/devcontainer/vita2hos:latest'
$port = (Resolve-Path $PSScriptRoot).Path
$sd = Join-Path $port 'dist\sd'
$managed = Join-Path $sd 'data\app\managed'
$dlls = 'mscorlib.dll', 'System.dll', 'Mono.Security.dll', 'System.Core.dll', 'System.Xml.dll', 'UnityEngine.dll',
        'Boo.Lang.dll', 'UnityScript.Lang.dll', 'P31RestKit.dll', 'Assembly-CSharp-firstpass.dll',
        'Assembly-UnityScript-firstpass.dll', 'Assembly-CSharp.dll', 'Assembly-UnityScript.dll'

function Step([string] $text) { Write-Host "`n== $text ($(Get-Date -Format HH:mm:ss))" -ForegroundColor Cyan }
function Fail([string] $text) { Write-Host "setup: $text" -ForegroundColor Red; exit 1 }

# Runs a shell command in the toolchain image, with the port at /work and the game data at /gamedata.
function Tool([string] $command, [string[]] $extra = @()) {
    docker run --rm -v "${port}:/work" -v "${GameData}:/gamedata" @extra -w /work $image sh -c $command
    if ($LASTEXITCODE -ne 0) { Fail "a step failed: $command" }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { Fail 'Docker is not installed or not on PATH.' }
docker info *> $null
if ($LASTEXITCODE -ne 0) { Fail 'Docker is installed but not running. Start Docker Desktop and try again.' }
New-Item -ItemType Directory -Force $GameData, $sd | Out-Null
$GameData = (Resolve-Path $GameData).Path

if (-not $ShadersOnly) {
    if (-not $Apk -or -not $Obb) { Fail 'give your game files: -Apk <file.apk> -Obb <file.obb>' }
    if (-not (Test-Path $Apk)) { Fail "no such APK: $Apk" }
    if (-not (Test-Path $Obb)) { Fail "no such OBB: $Obb" }
    $apkFile = Get-Item $Apk
    $obbFile = Get-Item $Obb

    # The files the port was made and tested with: versionName 1.0.40, versionCode 15.  Others may work,
    # but the libraries must match what the port patches, so say so before hours of work.
    Step '0/7 checking your files'
    $known = @{ $apkFile.FullName = 'e468b92901054d2fa0ac92908a97015424d17822'
                $obbFile.FullName = '7e7d910a8956f76d0f0d8756be853bcf6bf12b3d' }
    foreach ($f in $known.Keys) {
        $sha1 = (Get-FileHash $f -Algorithm SHA1).Hash.ToLower()
        if ($sha1 -eq $known[$f]) { "$(Split-Path $f -Leaf): matches the tested file" }
        else { Write-Host "$(Split-Path $f -Leaf): SHA-1 $sha1 is not the tested file (APK 1.0.40, versionCode 15); continuing" -ForegroundColor Yellow }
    }
    $inputs = @('-v', "$($apkFile.DirectoryName):/apk:ro", '-v', "$($obbFile.DirectoryName):/obb:ro")
    $apkIn = "'/apk/$($apkFile.Name)'"
    $obbIn = "'/obb/$($obbFile.Name)'"

    Step '1/7 extracting the APK and OBB'
    if (Test-Path (Join-Path $GameData 'extract_manifest.csv')) { 'already done' }
    else { Tool "python3 tools/extract.py $apkIn $obbIn /gamedata" $inputs }

    Step '2/7 copying the containers and libraries'
    if ((Test-Path "$sd\game.obb") -and (Test-Path "$sd\lib\libunity.so")) { 'already done' }
    else {
        $storeArg = if ($Store) { ' --store' } else { '' }
        Tool "python3 tools/package.py /gamedata $apkIn $obbIn dist/sd$storeArg" $inputs
    }

    Step '3/7 copying the managed DLLs'
    $source = Join-Path $GameData 'apk\assets\bin\Data\Managed'
    if (-not (Test-Path "$source\Assembly-CSharp.dll")) { Fail "no managed DLLs in ${source}: is this the right APK?" }
    New-Item -ItemType Directory -Force "$managed\mono\2.0" | Out-Null
    Copy-Item "$source\*.dll" $managed -Force
    Copy-Item "$managed\mscorlib.dll" "$managed\mono\2.0\" -Force

    $todo = @($dlls | Where-Object { -not (Test-Path "$managed\$_.so") -and -not (Test-Path "$managed\$_.s") })
    Step "4/7 compiling the game's C# ahead of time ($($todo.Count) of $($dlls.Count) DLLs left; this takes hours)"
    if ($todo.Count) {
        Tool 'sh tests/qemu/build.sh'
        docker build --platform linux/arm/v7 -q -t cod-harness (Join-Path $port 'tests\qemu') | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail 'could not build the ARM harness image (Docker needs ARM emulation, which Docker Desktop has)' }
        foreach ($d in $todo) {
            Write-Host "compiling $d ($(Get-Date -Format HH:mm:ss))"
            powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port 'tests\qemu\aot.ps1') $d 6000 |
                Where-Object { $_ -match 'Compiled|aot exit|Failed|rror' }
            if (-not (Test-Path "$managed\$d.s")) { Fail "the ahead-of-time compile of $d produced nothing; run setup again to retry it" }
        }
    } else { 'already done' }

    Step '5/7 linking the compiled code'
    Tool 'sh tests/qemu/aot_link.sh dist/sd/data/app/managed dist/sd/data/app/managed'
    foreach ($d in $dlls) { if (-not (Test-Path "$managed\$d.so")) { Fail "linking $d failed" } }
}

Step '6/7 building shaders.pak'
if (-not (Test-Path (Join-Path $GameData 'apk'))) { Fail 'run setup with -Apk and -Obb first' }
$seenArg = ''
$seenMount = @()
if ($Seen) {
    if (-not (Test-Path $Seen)) { Fail "no such folder: $Seen" }
    $seenMount = @('-v', "$((Resolve-Path $Seen).Path):/seen:ro")
    $seenArg = ' --seen /seen'
}
Tool ("export PATH=`$PATH:/opt/devkitpro/tools/bin; python3 tools/extract_shaders.py /gamedata/shaders/gles " +
      "/gamedata/apk /gamedata/obb$seenArg && python3 tools/translate_shaders.py /gamedata/shaders/gles " +
      "/gamedata/shaders/out dist/sd/shaders.pak") $seenMount

Step '7/7 gathering the SD card folder'
New-Item -ItemType Directory -Force "$Out\lib", "$Out\data\app\managed\mono\2.0" | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $ShadersOnly) {
    Copy-Item "$sd\game.apk", "$sd\game.obb" $Out -Force
    Copy-Item "$sd\lib\*.so" "$Out\lib" -Force
    foreach ($d in $dlls) { Copy-Item "$managed\$d", "$managed\$d.so" "$Out\data\app\managed" -Force }
    Copy-Item "$managed\mono\2.0\mscorlib.dll" "$Out\data\app\managed\mono\2.0" -Force
}
Copy-Item "$sd\shaders.pak" $Out -Force
$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host "`nDone: $Out ($([math]::Round($size / 1MB)) MB)" -ForegroundColor Green
Write-Host 'Copy its contents to sdmc:/switch/cod_st/ (next to cod_st.nro). Do not share this folder: it is your game.'
