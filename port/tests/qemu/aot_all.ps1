# Compiles every managed DLL in dependency order.  Slow: this is a compiler on an emulated CPU.
$dlls = 'mscorlib.dll', 'System.dll', 'Mono.Security.dll', 'System.Core.dll', 'System.Xml.dll', 'UnityEngine.dll',
        'Boo.Lang.dll', 'UnityScript.Lang.dll', 'P31RestKit.dll', 'Assembly-CSharp-firstpass.dll',
        'Assembly-UnityScript-firstpass.dll', 'Assembly-CSharp.dll', 'Assembly-UnityScript.dll'
foreach ($d in $dlls) {
    "===== $d $(Get-Date -Format HH:mm:ss)"
    powershell -NoProfile -File "$PSScriptRoot\aot.ps1" $d 3000 2>&1 | ForEach-Object { "$_" } |
        Where-Object { $_ -match 'Compiled|aot exit|returned|Failed|Error|error|Name|\.s ' }
}
"===== finished $(Get-Date -Format HH:mm:ss)"
