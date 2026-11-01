param([string]$Compiler='')
$ErrorActionPreference='Stop'
if(!$Compiler){$Compiler=Join-Path (Split-Path $PSScriptRoot -Parent) 'tools/InnoSetup/ISCC.exe'}
if(!(Test-Path -LiteralPath $Compiler)){
 $command=Get-Command ISCC.exe -ErrorAction SilentlyContinue
 if($command){$Compiler=$command.Source}else{throw 'Install Inno Setup and pass -Compiler with the full path to ISCC.exe.'}
}
& $Compiler (Join-Path $PSScriptRoot 'KMX6-V1.iss')
if($LASTEXITCODE){throw 'Installer compilation failed.'}
