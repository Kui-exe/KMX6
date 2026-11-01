param([switch]$InstallDriver,[switch]$ReconnectOnly,[string]$CacheDirectory=(Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'KMX6/Downloads'))
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(!$ReconnectOnly -and !$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
 $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'))
 if($InstallDriver){$arguments+='-InstallDriver'}
 $arguments+=@('-CacheDirectory',('"'+$CacheDirectory+'"'))
 $elevated=Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -PassThru
 $elevated.WaitForExit()
 exit $elevated.ExitCode
}
$setupMutex=[Threading.Mutex]::new($false,'Global\KMX6VirtualDeviceSetup')
if(!$setupMutex.WaitOne(0)){$setupMutex.Dispose();exit 3}
$logName=if($ReconnectOnly){'virtual-reconnect.txt'}else{'virtual-setup.txt'}
$logFile=Join-Path (Join-Path $projectRoot 'bin') $logName
Start-Transcript -Path $logFile -Force | Out-Null
try {
 $usbip=Join-Path $projectRoot 'tools\USBip\usbip.exe'
 if (!$ReconnectOnly -and $InstallDriver -and (!(Test-Path $usbip) -or !(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\usbip2_ude'))) {
  . (Join-Path $projectRoot 'Dependency-Downloads.ps1')
  $installer=Get-KMX6Dependency -Kind USBIP -CacheDirectory $CacheDirectory
  $signature=Get-AuthenticodeSignature -LiteralPath $installer
  if($signature.Status -ne 'Valid'){throw "Transport installer signature could not be validated: $($signature.Status). No installation performed."}
  # Official signed transport only. Never enables test signing or changes Secure Boot.
  $install=Start-Process -FilePath $installer -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="'+(Join-Path $projectRoot 'tools\USBip')+'"') -Wait -PassThru
  if($install.ExitCode -notin @(0,3010)){throw "USB/IP installer exited with $($install.ExitCode)"}
  if($install.ExitCode -eq 3010){Write-Host 'Windows restart required to finish virtual audio installation.';exit 3010}
 }
 if(!(Test-Path $usbip)) {
  foreach($candidate in @('C:\Program Files\USBip\usbip.exe','C:\Program Files\usbip-win2\usbip.exe')){if(Test-Path $candidate){$usbip=$candidate;break}}
 }
 if(!(Test-Path $usbip)){throw 'USB/IP transport is not installed. Run this script with -InstallDriver from an administrator PowerShell.'}
 $hostPath=Join-Path $projectRoot 'bin\KMX6CableHost.exe'
 if(!(Get-Process KMX6CableHost -ErrorAction SilentlyContinue)){Start-Process -FilePath $hostPath -WorkingDirectory (Join-Path $projectRoot 'bin') -WindowStyle Hidden}
 Start-Sleep -Milliseconds 500
 & $usbip -t 3241 list -r 127.0.0.1
 if($LASTEXITCODE){throw 'Could not reach the KMX6 endpoint host.'}
 # Attach only this project's six local endpoints; no other ports are detached.
 $ports=(& $usbip port 2>&1 | Out-String)
 # Keep one attachment per KMX6 serial; concurrent/restarted setup must not
 # leave two Windows endpoints feeding the same virtual device.
 $attachments=foreach($block in [regex]::Matches($ports,'(?ms)^Port\s+(\d+):(?:(?!^Port\s+\d+:).)*')){
  $device=[regex]::Match($block.Value,'usbip://127\.0\.0\.1:3241/1-([1-6])\b')
  if($device.Success){[pscustomobject]@{Port=[int]$block.Groups[1].Value;Device=$device.Groups[1].Value;NeedsRename=(($block.Value -notmatch 'serial:\s+KMX6\d{3}\b') -or ($block.Value -notmatch '\(ffff:cb0[1-6]\)'))}}
 }
 foreach($old in ($attachments | Where-Object { !$ReconnectOnly -and $_.NeedsRename })){& $usbip detach -p $old.Port;if($LASTEXITCODE){throw "Could not migrate KMX6 endpoint $($old.Device)"}}
 $attachments=@($attachments | Where-Object { $ReconnectOnly -or !$_.NeedsRename })
 foreach($group in ($attachments | Where-Object { !$ReconnectOnly } | Group-Object Device)){foreach($duplicate in ($group.Group | Sort-Object Port -Descending | Select-Object -Skip 1)){& $usbip detach -p $duplicate.Port;if($LASTEXITCODE){throw "Could not detach duplicate KMX6 port $($duplicate.Port)"}}}
 $ports=(& $usbip port 2>&1 | Out-String)
 foreach($number in 1..6){
  if($ports -notmatch ('usbip://127\.0\.0\.1:3241/1-'+$number+'\b')){
   & $usbip -t 3241 attach -r 127.0.0.1 -b "1-$number" --serial ('KMX6{0:000}' -f $number) --once --receive-mode zero-copy
   if($LASTEXITCODE){throw "Could not attach KMX6 endpoint $number"}
  }
 }
 if(!$ReconnectOnly){
  $persistHelper=Join-Path $projectRoot 'tools/USBip/KMX6DeviceSetup.exe'
  if(!(Test-Path $persistHelper)){throw 'KMX6 device setup helper is missing. Restore it from the application package.'}
  & $persistHelper --persist
  if($LASTEXITCODE){throw 'Could not save automatic KMX6 reconnection. See the device setup log.'}
  'Persistent KMX6 attachments configured.' | Set-Content -LiteralPath (Join-Path $projectRoot 'bin/virtual-install-state.txt')
 }
 if($ReconnectOnly){Start-Sleep -Milliseconds 500}else{Start-Sleep -Seconds 3}
 $renameScript=Join-Path $projectRoot 'Rename-AudioEndpoints.ps1'
 if(!$ReconnectOnly -and (Test-Path $renameScript)){try{Write-Host (& $renameScript)}catch{Write-Warning $_.Exception.Message}}
 Write-Host 'KMX6 Computer 1 / Computer 2 playback and Mic B1 / B2 / B3 / B4 recording are attached. Devices will reconnect automatically.'
} catch {Write-Host ('KMX6 device setup failed: '+$_.Exception.Message);exit 1} finally {Stop-Transcript | Out-Null;$setupMutex.ReleaseMutex();$setupMutex.Dispose()}


