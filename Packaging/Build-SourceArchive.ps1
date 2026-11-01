param([string]$ProjectRoot=(Split-Path $PSScriptRoot -Parent),[string]$Destination='')
$ErrorActionPreference='Stop'
if(!$Destination){$Destination=Join-Path $ProjectRoot 'dist/KMX6-V1-Source.zip'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force|Out-Null
$paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($name in @('CMakeLists.txt','LICENSE','README.md','CONTRIBUTING.md','THIRD-PARTY-NOTICES.md','.gitignore','.gitattributes','.gitmodules','Launch.cmd')){$file=Join-Path $ProjectRoot $name;if(Test-Path -LiteralPath $file){$null=$paths.Add($file)}}
foreach($file in (Get-ChildItem -LiteralPath $ProjectRoot -Filter '*.ps1' -File)){$null=$paths.Add($file.FullName)}
foreach($folder in @('Source','CableHost','Assets','docs','Packaging','third_party/JUCE','third_party/miniaudio','third_party/Virtual-Cables/src')){
 foreach($file in (Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $folder) -Recurse -File)){
  $rel=$file.FullName.Substring($ProjectRoot.Length+1).Replace('\','/')
  if($rel -match '(^|/)\.git(/|$)|(^|/)(build|node_modules|__pycache__)(/|$)' -or $file.Name -match '\.before-|\.(pdb|obj|ilk)$'){continue}
  $null=$paths.Add($file.FullName)
 }
}
foreach($folder in @('tools/USBip/include','tools/USBip/lib')){foreach($file in (Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $folder) -Recurse -File)){$null=$paths.Add($file.FullName)}}
foreach($name in @('third_party/Virtual-Cables/LICENSE.txt','third_party/Virtual-Cables/README.md','tools/USBip/libusbip.dll','tools/USBip/resources.dll','tools/USBip/usbip.exe','tools/USBip/LICENSE.txt','tools/USBip/Readme.md','tools/go/LICENSE','tools/go/PATENTS')){$null=$paths.Add((Join-Path $ProjectRoot $name))}
$stream=[IO.File]::Open($Destination,[IO.FileMode]::Create)
$zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
try{foreach($file in ($paths|Sort-Object)){$relative=$file.Substring($ProjectRoot.Length+1).Replace('\','/');$null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file,('KMX6/'+$relative),[IO.Compression.CompressionLevel]::Optimal)}}finally{$zip.Dispose();$stream.Dispose()}
Write-Host "Created complete source archive: $Destination"
