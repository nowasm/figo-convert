# Stages the Figo Prefab Importer package content into build_unityplugin/.
# Copy the resulting FigoPrefabImporter folder into a Unity project's Assets/
# and export it from there via the Asset Store Publishing Tools.
#
#   powershell -File tools/pack_unity_plugin.ps1
#
# The package bundles BOTH platform converters in Editor/Bin:
#   figo2unity.exe  (Windows x64)   figo2unity  (macOS mach-o)
# Each comes from a fresh build\ artifact when present, otherwise from the
# committed unity-plugin\prebuilt\<platform>\ copy. After rebuilding a
# converter on its native platform, refresh the prebuilt copy and commit it:
#   Windows: Copy-Item build\figo2unity.exe unity-plugin\prebuilt\win-x64\
#   macOS:   cp build/figo2unity unity-plugin/prebuilt/macos/
# Build notes for the macOS binary: docs/agent-memory/unity-plugin-mac-support.md
$ErrorActionPreference = "Stop"
$repo  = Split-Path $PSScriptRoot -Parent
$src   = Join-Path $repo "unity-plugin\FigoPrefabImporter"
$out   = Join-Path $repo "build_unityplugin\FigoPrefabImporter"

# Per platform: prefer the freshly built binary, fall back to the committed
# prebuilt copy (so a Windows machine can pack the macOS binary and vice versa).
function Resolve-Converter($built, $prebuilt) {
    if (Test-Path $built) { return $built }
    if (Test-Path $prebuilt) { return $prebuilt }
    return $null
}
$exeWin = Resolve-Converter (Join-Path $repo "build\figo2unity.exe") `
                            (Join-Path $repo "unity-plugin\prebuilt\win-x64\figo2unity.exe")
$exeMac = Resolve-Converter (Join-Path $repo "build\figo2unity") `
                            (Join-Path $repo "unity-plugin\prebuilt\macos\figo2unity")

if (-not $exeWin) { throw "figo2unity.exe not found — build the repo first (see CLAUDE.md) or commit unity-plugin\prebuilt\win-x64\figo2unity.exe" }
if (-not $exeMac) { Write-Warning "macOS converter missing (unity-plugin\prebuilt\macos\figo2unity) — packing a Windows-only plugin" }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

# plugin source (Editor scripts + docs)
Copy-Item -Recurse "$src\*" $out

# converter binaries
New-Item -ItemType Directory -Force "$out\Editor\Bin" | Out-Null
Copy-Item $exeWin "$out\Editor\Bin\figo2unity.exe"
if ($exeMac) { Copy-Item $exeMac "$out\Editor\Bin\figo2unity" }

# sample design (starfall: menu + settings, pre-captured canvas.json)
New-Item -ItemType Directory -Force "$out\Samples" | Out-Null
Copy-Item (Join-Path $repo "examples\html\starfall_menu.canvas.json") "$out\Samples\"
Copy-Item -Recurse (Join-Path $repo "examples\html\images") "$out\Samples\images"

Write-Host "staged -> $out"
Write-Host ("windows converter: " + $exeWin)
Write-Host ("macos converter:   " + ($(if ($exeMac) { $exeMac } else { "(none)" })))
Get-ChildItem -Recurse $out -File | Measure-Object -Property Length -Sum |
    ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }
