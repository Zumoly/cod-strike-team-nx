# Ahead-of-time compiles one managed DLL with the game's own libmono on emulated ARM.
# Output: dist/sd/data/app/managed/<dll>.s   usage: aot.ps1 <dll> [seconds]
param([string] $Dll = 'mscorlib.dll', [int] $Limit = 3000)
$port = (Resolve-Path "$PSScriptRoot\..\..").Path
$job = Join-Path $port 'dist\sd\aot_job'
[IO.File]::WriteAllText($job, "$Dll`n")
$name = 'cod-aot-' + ($Dll -replace '[^A-Za-z0-9]', '').ToLower()
docker rm -f $name 2>$null | Out-Null
docker run -d --name $name --ulimit core=0 --platform linux/arm/v7 -v "${port}\dist\sd:/sd" cod-harness | Out-Null
$deadline = (Get-Date).AddSeconds($Limit)
while ((Get-Date) -lt $deadline) {
    if ((docker inspect -f '{{.State.Running}}' $name) -ne 'true') { break }
    if ((docker logs --tail 1 $name 2>&1 | Out-String) -match 'uncaught target signal') { Start-Sleep -Seconds 1; break }
    Start-Sleep -Seconds 5
}
docker logs --tail 40 $name 2>&1
"aot exit=$(docker inspect -f '{{.State.ExitCode}}' $name)"
docker rm -f $name | Out-Null
Remove-Item $job -ErrorAction SilentlyContinue
Get-Item "$port\dist\sd\data\app\managed\$Dll.s" -ErrorAction SilentlyContinue | Select-Object Name, Length
