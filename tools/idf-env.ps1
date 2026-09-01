# Activates ESP-IDF in the current PowerShell session.
#
#   . .\tools\idf-env.ps1
#   idf.py -p COM6 monitor
#
# The Start Menu shortcut "ESP-IDF 5.5 PowerShell" does the same thing and is
# the easier route for a fresh terminal. This script exists for the case where
# you already have a shell open in the project.
#
# Note the dot at the start: without it the script runs in a child scope and
# the PATH changes vanish when it exits.

$ErrorActionPreference = 'Stop'

$idfPath   = 'C:\Espressif\frameworks\esp-idf-v5.5'
$toolsPath = 'C:\Espressif'

if (-not (Test-Path $idfPath)) {
    throw "ESP-IDF not found at $idfPath. Edit tools/idf-env.ps1 if yours lives elsewhere."
}

$env:IDF_PATH       = $idfPath
$env:IDF_TOOLS_PATH = $toolsPath

# export.ps1 invokes whatever `python` is first on PATH, and the copy that owns
# the ESP-IDF virtualenv is not necessarily it. Put the bundled interpreter in
# front so the version check matches the venv that was actually installed.
$idfPython = Get-ChildItem "$env:USERPROFILE\.espressif\tools\idf-python\*" -Directory |
             Sort-Object Name -Descending |
             Select-Object -First 1

if ($idfPython) {
    $env:PATH = "$($idfPython.FullName);$($idfPython.FullName)\Scripts;$env:PATH"
}

& "$env:IDF_PATH\export.ps1" | Out-Null

Write-Host "ESP-IDF ready: $(idf.py --version)"
