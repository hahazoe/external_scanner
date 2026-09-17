param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Lua", "Dll")]
    [string]$Mode,
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$Output
)

$ErrorActionPreference = "Stop"

function Write-CArray {
    param(
        [byte[]]$Bytes,
        [string]$SizeName,
        [string]$ArrayName,
        [string]$Path
    )
    $builder = New-Object Text.StringBuilder
    [void]$builder.AppendLine("#ifndef ${ArrayName}_H")
    [void]$builder.AppendLine("#define ${ArrayName}_H")
    [void]$builder.AppendLine("#define $SizeName $($Bytes.Length)")
    [void]$builder.AppendLine("static const unsigned char $ArrayName[$($Bytes.Length)] = {")
    for ($i = 0; $i -lt $Bytes.Length; $i++) {
        if (($i % 12) -eq 0) {
            [void]$builder.Append("  ")
        }
        [void]$builder.Append(("0x{0:X2}," -f $Bytes[$i]))
        if (($i % 12) -eq 11 -or $i -eq $Bytes.Length - 1) {
            [void]$builder.AppendLine()
        }
    }
    [void]$builder.AppendLine("};")
    [void]$builder.AppendLine("#endif")
    [IO.File]::WriteAllText($Path, $builder.ToString(), [Text.Encoding]::ASCII)
}

if ($Mode -eq "Lua") {
    $hookPath = Join-Path $SourceRoot "injection\scripts\lua_hook.lua"
    $bootstrapPath = Join-Path $SourceRoot "injection\scripts\bootstrap.lua"
    $hook = [IO.File]::ReadAllText($hookPath)
    $bootstrap = [IO.File]::ReadAllText($bootstrapPath)
    $combined = "local hookModule = (function()`n$hook`nend)()`n$bootstrap"
    $bytes = [Text.Encoding]::UTF8.GetBytes($combined)
    Write-CArray $bytes "BOOTSTRAP_LUA_SIZE" "bootstrap_lua_data" $Output
}
else {
    $dllPath = Join-Path $SourceRoot "build\generated\inject_exec.dll"
    $bytes = [IO.File]::ReadAllBytes($dllPath)
    Write-CArray $bytes "INJECT_DLL_SIZE" "inject_dll_data" $Output
}
