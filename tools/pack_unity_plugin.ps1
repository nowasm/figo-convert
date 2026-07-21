# Stages the Figo Prefab Importer package content into build_unityplugin/.
# Copy the resulting FigoPrefabImporter folder into a Unity project's Assets/
# and export it from there via the Asset Store Publishing Tools.
#
#   powershell -File tools/pack_unity_plugin.ps1
#
# The package ships WITHOUT converter binaries: the plugin
# downloads the per-platform figo2unity from the public figo repo on first
# convert (https://raw.githubusercontent.com/nowasm/figo/master/prebuild/,
# cached in the user project's Library/FigoPrefabImporter/). To refresh the
# published binaries, rebuild and commit them to ../figo/prebuild/
# (win-x64/figo2unity.exe, macos/figo2unity universal — build notes:
# docs/agent-memory/unity-plugin-mac-support.md).
$ErrorActionPreference = "Stop"
$repo  = Split-Path $PSScriptRoot -Parent
$src   = Join-Path $repo "unity-plugin\unityproj\Assets\FigoPrefabImporter"
$out   = Join-Path $repo "build_unityplugin\FigoPrefabImporter"

if (-not (Test-Path $src)) { throw "plugin source not found: $src" }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

# plugin source (Editor scripts + docs + .meta files for stable GUIDs)
Copy-Item -Recurse "$src\*" $out

# no binaries in the package — fail loudly if a stray Editor/Bin sneaks back
if (Test-Path "$out\Editor\Bin") { throw "Editor\Bin must not be packaged (converter is downloaded at first use) — remove it from $src" }

# sample design (starfall: menu + settings, pre-captured canvas.json) —
# normally already present in the source project (with .meta); refresh the
# payload from examples/ without disturbing existing .meta files
New-Item -ItemType Directory -Force "$out\Samples\images" | Out-Null
Copy-Item (Join-Path $repo "examples\html\starfall_menu.canvas.json") "$out\Samples\" -Force
Copy-Item (Join-Path $repo "examples\html\images\*") "$out\Samples\images\" -Force

Write-Host "staged -> $out"
Write-Host "converter: downloaded at first use from https://raw.githubusercontent.com/nowasm/figo/master/prebuild/"
Get-ChildItem -Recurse $out -File | Measure-Object -Property Length -Sum |
    ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }
