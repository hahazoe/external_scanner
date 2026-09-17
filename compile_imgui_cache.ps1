param(
    [Parameter(Mandatory = $true)][string]$SrcRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$ImguiDir,
    [Parameter(Mandatory = $true)][string]$ImplotDir
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$compat = Join-Path $SrcRoot "debugview\mingw_compat.h"
$pairs = @(
    @( (Join-Path $ImguiDir "imgui.cpp"), (Join-Path $OutDir "imgui.o") ),
    @( (Join-Path $ImguiDir "imgui_draw.cpp"), (Join-Path $OutDir "imgui_draw.o") ),
    @( (Join-Path $ImguiDir "imgui_tables.cpp"), (Join-Path $OutDir "imgui_tables.o") ),
    @( (Join-Path $ImguiDir "imgui_widgets.cpp"), (Join-Path $OutDir "imgui_widgets.o") ),
    @( (Join-Path $ImguiDir "backends\imgui_impl_win32.cpp"), (Join-Path $OutDir "imgui_impl_win32.o") ),
    @( (Join-Path $ImguiDir "backends\imgui_impl_dx9.cpp"), (Join-Path $OutDir "imgui_impl_dx9.o") ),
    @( (Join-Path $ImplotDir "implot.cpp"), (Join-Path $OutDir "implot.o") ),
    @( (Join-Path $ImplotDir "implot_items.cpp"), (Join-Path $OutDir "implot_items.o") )
)

foreach ($pair in $pairs) {
    $src = $pair[0]
    $obj = $pair[1]
    $need = $true
    if (Test-Path $obj) {
        if ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime -and
            (Get-Item $compat).LastWriteTime -le (Get-Item $obj).LastWriteTime) {
            $need = $false
        }
    }
    if ($need) {
        Write-Host ("compile " + [IO.Path]::GetFileName($src))
        & g++ -m32 -O2 -std=gnu++11 -Wall -DCOCOS_DEBUG_IMGUI_NO_DWMAPI `
            -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD `
            -include $compat `
            -I $ImguiDir -I (Join-Path $ImguiDir "backends") -I $ImplotDir `
            -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } else {
        Write-Host ("cache  " + [IO.Path]::GetFileName($obj))
    }
}
