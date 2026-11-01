# Download only known dependency packages; never execute an unverified download.
$KMX6MinimumRuntime=[Version]'14.51.36247.0'
function Test-KMX6Dependency([string]$Path,[string]$Kind){
 if(!(Test-Path -LiteralPath $Path)){return $false}
 try {
  $signature=Get-AuthenticodeSignature -LiteralPath $Path
  if($signature.Status -ne 'Valid'){return $false}
  if($Kind -eq 'USBIP'){
   return ($signature.SignerCertificate.Subject -match 'Cloudyne Systems') -and ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq '38CAD6D4432B52D5BB9409D9AD03B72FDFFC4ADA4CD3A48FBECA1A2752A8518A')
  }
  if($signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation'){return $false}
  $version=[regex]::Match((Get-Item -LiteralPath $Path).VersionInfo.ProductVersion,'\d+\.\d+\.\d+(\.\d+)?').Value
  return $version -and ([Version]$version -ge $KMX6MinimumRuntime)
 }catch{return $false}
}
function Get-KMX6Dependency {
 param([Parameter(Mandatory=$true)][ValidateSet('Runtime','USBIP')][string]$Kind,[string]$CacheDirectory=(Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'KMX6/Downloads'))
 $ErrorActionPreference='Stop'
 New-Item -ItemType Directory -Path $CacheDirectory -Force | Out-Null
 if($Kind -eq 'Runtime'){$name='VC_redist.x64.exe';$uri='https://aka.ms/vc14/vc_redist.x64.exe'}
 else{$name='USBip-0.9.8.1-x64.exe';$uri='https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe'}
 $target=Join-Path $CacheDirectory $name
 if(Test-KMX6Dependency $target $Kind){Write-Host "Using previously verified $name";return $target}
 $temporary=Join-Path $CacheDirectory ([Guid]::NewGuid().ToString()+'.download.exe')
 [Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
 try {
  Write-Host "Downloading required $name from $uri"
  for($attempt=1;$attempt -le 2;$attempt++){
   try{Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $temporary -TimeoutSec 120;break}
   catch{if($attempt -eq 2){throw "Could not download $name. Check your Internet connection and run Repair KMX6 Audio to retry. $($_.Exception.Message)"};Start-Sleep -Seconds 1}
  }
  if(!(Test-KMX6Dependency $temporary $Kind)){throw "The downloaded $name failed publisher, version, or checksum verification. It was not installed."}
  Move-Item -LiteralPath $temporary -Destination $target -Force
  Write-Host "Verified $name"
  return $target
 }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
