# Builds the harness and runs it on emulated ARM against dist/test, a copy kept apart from
# dist/sd so test runs cannot collide with an AOT compile that is using dist/sd.
# usage: run.ps1 [seconds] [-Strace]   The limit also covers QEMU stalling after a guest crash.
param([int] $Limit = 150, [switch] $Strace, [string] $Level = '')
$port = (Resolve-Path "$PSScriptRoot\..\..").Path
$image = 'ghcr.io/vita2hos/devcontainer/vita2hos:latest'
docker run --rm -v "${port}:/work" -w /work $image sh tests/qemu/build.sh
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
docker build --platform linux/arm/v7 -q -t cod-harness-test "$PSScriptRoot" | Out-Null

$test = Join-Path $port 'dist\test'
$managed = Join-Path $test 'data\app\managed'
if (-not (Test-Path "$managed\mscorlib.dll")) {
    New-Item -ItemType Directory -Force "$test\lib", "$managed\mono\2.0" | Out-Null
    Copy-Item "$port\dist\sd\lib\*.so" "$test\lib\"
    Copy-Item "$port\dist\sd\data\app\managed\*.dll" $managed
    Copy-Item "$managed\mscorlib.dll" "$managed\mono\2.0\"
}
Copy-Item "$port\dist\sd\shaders.pak" $test -Force
Remove-Item "$test\load_level" -ErrorAction SilentlyContinue
if ($Level) { [IO.File]::WriteAllText("$test\load_level", $Level) }
foreach ($f in 'game.apk', 'game.obb') {
    if (-not (Test-Path "$test\$f")) { New-Item -ItemType File "$test\$f" | Out-Null }
}

$name = 'cod-harness-run'
$envArgs = @()
if ($Strace) { $envArgs = @('-e', 'QEMU_STRACE=1') }
docker rm -f $name 2>$null | Out-Null
docker run -d --name $name --ulimit core=0 --platform linux/arm/v7 @envArgs -v "${test}:/sd" `
    -v "${port}\dist\sd\game.apk:/sd/game.apk:ro" -v "${port}\dist\sd\game.obb:/sd/game.obb:ro" cod-harness-test | Out-Null
$deadline = (Get-Date).AddSeconds($Limit)
while ((Get-Date) -lt $deadline) {
    if ((docker inspect -f '{{.State.Running}}' $name) -ne 'true') { break }
    $tail = docker logs --tail 1 $name 2>&1 | Out-String
    if ($tail -match 'uncaught target signal') { Start-Sleep -Seconds 1; break }
    Start-Sleep -Seconds 2
}
$running = (docker inspect -f '{{.State.Running}}' $name) -eq 'true'
docker logs $name
if ($running) { "HARNESS STOPPED at limit or after a guest crash (${Limit}s)" }
$exit = docker inspect -f '{{.State.ExitCode}}' $name
"harness exit=$exit"
docker rm -f $name | Out-Null
