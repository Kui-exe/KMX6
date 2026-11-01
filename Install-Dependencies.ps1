param([switch]$Elevated,[string]$CacheDirectory=(Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'KMX6/Downloads'))
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
. (Join-Path $root 'Dependency-Downloads.ps1')
$setup=Join-Path $root 'Setup-VirtualIO.ps1'
$log=Join-Path $root 'bin/dependency-setup.txt'
function RuntimeNeeded {
 $installed=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue
 if(!$installed -or !$installed.Installed){return $true}
 try{return [Version]($installed.Version.TrimStart('v')) -lt $KMX6MinimumRuntime}catch{return $true}
}
try {
 $needsRuntime=RuntimeNeeded
 $needsDriver=!(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\usbip2_ude')
 $helper=Join-Path $root 'tools/USBip/KMX6DeviceSetup.exe'
 $needsDevices=$true
 if(!$needsRuntime -and !$needsDriver){& $helper --check | Out-Null;$needsDevices=$LASTEXITCODE -ne 0}
 if(!$needsRuntime -and !$needsDriver -and !$needsDevices){'Required runtime and virtual audio are already configured. No downloads or installations were needed.' | Set-Content -LiteralPath $log;exit 0}
 $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
 if(!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
  $args=@('-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',('"'+$PSCommandPath+'"'),'-Elevated','-CacheDirectory',('"'+$CacheDirectory+'"'))
  try{$child=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -Verb RunAs -WindowStyle Hidden -ArgumentList $args -PassThru;$child.WaitForExit();exit $child.ExitCode}catch{'Administrator approval was cancelled. Use Repair KMX6 Audio to finish setup.' | Set-Content -LiteralPath $log;exit 1223}
 }
 Start-Transcript -LiteralPath $log -Force | Out-Null
 $restart=$false
 try {
  if($needsRuntime){
   $runtime=Get-KMX6Dependency -Kind Runtime -CacheDirectory $CacheDirectory
   if(!(Test-KMX6Dependency $runtime Runtime)){throw 'Runtime verification failed immediately before installation.'}
   $install=Start-Process -FilePath $runtime -ArgumentList '/install','/quiet','/norestart' -WindowStyle Hidden -PassThru;$install.WaitForExit()
   if($install.ExitCode -notin @(0,1638,3010)){throw "Microsoft runtime installation failed ($($install.ExitCode))."}
   $restart=$install.ExitCode -eq 3010
   if(RuntimeNeeded){if($restart){exit 3010};throw 'The required Microsoft runtime is still unavailable.'}
  }
  $args=@('-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',('"'+$setup+'"'),'-InstallDriver','-CacheDirectory',('"'+$CacheDirectory+'"'))
  $audio=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $args -WindowStyle Hidden -PassThru;$audio.WaitForExit()
  if($audio.ExitCode -eq 3010){$restart=$true}elseif($audio.ExitCode -ne 0){throw "Virtual audio setup failed ($($audio.ExitCode)). See virtual-setup.txt in the bin folder."}
  if($restart){exit 3010}
  Write-Host 'KMX6 runtime and all six virtual devices are configured.'
 }finally{Stop-Transcript | Out-Null}
 exit 0
}catch{('Setup failed: '+$_.Exception.Message) | Out-File -LiteralPath $log -Append;exit 1}
