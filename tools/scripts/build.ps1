$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio= & $vswhere -latest -products '*' -property installationPath
$cmake=Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path $cmake)) { throw 'Visual Studio CMake tools were not found.' }
& $cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE) {throw 'Configuration failed'}
& $cmake --build (Join-Path $projectRoot 'build') --config Release --parallel 4
if ($LASTEXITCODE) {throw 'Build failed'}
New-Item -ItemType Directory (Join-Path $projectRoot 'bin') -Force | Out-Null
Copy-Item (Join-Path $projectRoot 'build\KMX6_artefacts\Release\KMX6.exe') (Join-Path $projectRoot 'bin\KMX6.exe') -Force
Copy-Item (Join-Path $projectRoot 'build\Release\KMX6DeviceSetup.exe') (Join-Path $projectRoot 'tools\USBip\KMX6DeviceSetup.exe') -Force
Write-Host 'Built bin\KMX6.exe'

