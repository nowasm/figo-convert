# Stages the Figo Prefab Importer package content into build_unityplugin/.
# Copy the resulting FigoPrefabImporter folder into a Unity project's Assets/
# and export it from there via the Asset Store Publishing Tools.
#
#   powershell -File tools/pack_unity_plugin.ps1
#
$ErrorActionPreference = "Stop"
$repo  = Split-Path $PSScriptRoot -Parent
$src   = Join-Path $repo "unity-plugin\FigoPrefabImporter"
$exe   = Join-Path $repo "build\figo2unity.exe"
$out   = Join-Path $repo "build_unityplugin\FigoPrefabImporter"

if (-not (Test-Path $exe)) { throw "figo2unity.exe not built — build the repo first (see CLAUDE.md)" }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

# plugin source (Editor scripts + docs)
Copy-Item -Recurse "$src\*" $out

# converter binary
New-Item -ItemType Directory -Force "$out\Editor\Bin" | Out-Null
Copy-Item $exe "$out\Editor\Bin\figo2unity.exe"

# sample design (starfall: menu + settings, pre-captured canvas.json)
New-Item -ItemType Directory -Force "$out\Samples" | Out-Null
Copy-Item (Join-Path $repo "examples\html\starfall_menu.canvas.json") "$out\Samples\"
Copy-Item -Recurse (Join-Path $repo "examples\html\images") "$out\Samples\images"

Write-Host "staged -> $out"
Get-ChildItem -Recurse $out -File | Measure-Object -Property Length -Sum |
    ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }
