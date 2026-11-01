param([string]$OutputPath="")
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$hostOutput=if($OutputPath){$OutputPath}else{Join-Path $projectRoot 'bin\KMX6CableHost.exe'}
$module=Join-Path $projectRoot 'third_party\Virtual-Cables\src'
$entry=Join-Path $module 'cmd\kmx6cables'
New-Item -ItemType Directory $entry,(Join-Path $projectRoot 'bin') -Force | Out-Null
Copy-Item (Join-Path $projectRoot 'CableHost\main.go') (Join-Path $entry 'main.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\bridge_test.go') (Join-Path $entry 'bridge_test.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\directions.go'),(Join-Path $projectRoot 'CableHost\bridge.go') $entry -Force
Copy-Item (Join-Path $projectRoot 'CableHost\scheduling_windows.go') (Join-Path $module 'internal\usbip\kmx6_scheduling_windows.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\playback.go') (Join-Path $entry 'playback.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\ring.go') (Join-Path $module 'internal\audio\ring.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\ring_notify_test.go') (Join-Path $module 'internal\audio\kmx6_notify_test.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\shared_pcm_windows.go') (Join-Path $module 'internal\usbip\kmx6_shared_pcm_windows.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\shared_pcm_test.go') (Join-Path $module 'internal\usbip\kmx6_shared_pcm_test.go') -Force
$timingFile=Join-Path $module 'internal\usbip\server.go'
$timing=[IO.File]::ReadAllText($timingFile)
$old='if base.Before(now) {'
$new='if base.IsZero() || base.Before(now.Add(-250*time.Millisecond)) {'
if($timing.Contains($old)){$timing=$timing.Replace($old,$new)}
elseif(!$timing.Contains($new)){throw 'Unexpected upstream clock implementation; timing patch needs review.'}
$control='data, status := device.HandleControl(setup, submit.out)'
if(!$timing.Contains('state.resetIsoTimeline')){
 $replacement=$control+"`n"+'        if status == 0 && setup.RequestType == 0x01 && setup.Request == uac1.RequestSetInterface { state.resetIsoTimeline(uint32(setup.Index)) }'
 if(!$timing.Contains($control)){throw 'Unexpected upstream control implementation'}
 $timing=$timing.Replace($control,$replacement)
}
[IO.File]::WriteAllText($timingFile,$timing)
$deviceFile=Join-Path $module 'internal\uac1\device.go'
$device=[IO.File]::ReadAllText($deviceFile)
if(!$device.Contains('CaptureOnly bool')){
 $device=$device.Replace('type Device struct {',"type Device struct {`n    CaptureOnly bool`n    PlaybackOnly bool")
 $device=$device.Replace('return d.configuration == 1 && d.altSetting[1] == 1','return !d.CaptureOnly && d.configuration == 1 && d.altSetting[1] == 1')
 $device=$device.Replace('return d.configuration == 1 && d.altSetting[2] == 1',"if d.CaptureOnly {return d.configuration == 1 && d.altSetting[1] == 1}`n    return !d.PlaybackOnly && d.configuration == 1 && d.altSetting[2] == 1")
 [IO.File]::WriteAllText($deviceFile,$device)
}
$timing=[IO.File]::ReadAllText($timingFile)
if(!$timing.Contains('if device.CaptureOnly')){$timing=$timing.Replace('state.resetIsoTimeline(uint32(setup.Index))','if device.CaptureOnly { state.resetIsoTimeline(2) } else { state.resetIsoTimeline(uint32(setup.Index)) }')}
$timing=$timing.Replace('uint8(3), // interfaces','uint8(d.Descriptors.Config[4]), // interfaces')
$timing=$timing.Replace('for _, iface := range interfaces {','for _, iface := range interfaces[:int(d.Descriptors.Config[4])] {')
[IO.File]::WriteAllText($timingFile,$timing)
if(!$timing.Contains('enqueueKMX6Iso')){
 $timing=[regex]::Replace($timing,'(isoNext\s+map\[uint32\]time\.Time)','$1'+"`n"+'    kmx6Workers map[uint32]chan kmx6IsoJob')
 $ordered='state.enqueueKMX6Iso(urbCtx, requestCtx, requestCancel, submit, func(ctx context.Context, job parsedSubmit) error { return s.processSubmit(ctx, state, device, job) })'
 $timing=[regex]::Replace($timing,'go func\(job parsedSubmit\) \{.*?\}\(submit\)',$ordered,[Text.RegularExpressions.RegexOptions]::Singleline)
 if(!$timing.Contains('enqueueKMX6Iso')){throw 'Could not apply USB endpoint ordering patch'}
 [IO.File]::WriteAllText($timingFile,$timing)
}
$timing=[IO.File]::ReadAllText($timingFile)
if(!$timing.Contains('playbackStaged')){
 $timing=[regex]::Replace($timing,'(?m)^(\s*completeAt\s+time\.Time)\r?$','$1'+"`n"+'    playbackStaged bool')
 $timing=$timing.Replace('s.traceSubmit(device, submit)','stageKMX6Playback(device, &submit)'+"`n"+'            s.traceSubmit(device, submit)')
 $timing=$timing.Replace('if device.PlaybackActive() {','if !submit.playbackStaged && device.PlaybackActive() {')
 [IO.File]::WriteAllText($timingFile,$timing)
}
$timing=[IO.File]::ReadAllText($timingFile)
if(!$timing.Contains('releaseReceiver := BeginAudioScheduling')){
 $timing=$timing.Replace('func (s *Server) handleURBs(ctx context.Context, r *bufio.Reader, w io.Writer, device *uac1.Device) {','func (s *Server) handleURBs(ctx context.Context, r *bufio.Reader, w io.Writer, device *uac1.Device) {'+"`n"+'    releaseReceiver := BeginAudioScheduling();defer releaseReceiver()')
 [IO.File]::WriteAllText($timingFile,$timing)
}
Copy-Item (Join-Path $projectRoot 'CableHost\tcp_windows.go') (Join-Path $module 'internal\usbip\kmx6_tcp_windows.go') -Force
$timing=[IO.File]::ReadAllText($timingFile)
if(!$timing.Contains('TuneAudioTCP(tcp)')){
 $timing=$timing.Replace('_ = tcp.SetNoDelay(true)','_ = tcp.SetNoDelay(true)'+"`n"+'            if err:=TuneAudioTCP(tcp);err!=nil{s.Logger.Printf("Immediate ACK unavailable: %v",err)}')
 [IO.File]::WriteAllText($timingFile,$timing)
}
Copy-Item (Join-Path $projectRoot 'CableHost\timing_test.go') (Join-Path $module 'internal\usbip\kmx6_timing_test.go') -Force
Copy-Item (Join-Path $projectRoot 'CableHost\clock.go') (Join-Path $module 'internal\usbip\kmx6_clock.go') -Force
$env:GOCACHE=Join-Path $projectRoot 'tools\go-cache'
$env:GOMODCACHE=Join-Path $projectRoot 'tools\go-mod'
$go=Join-Path $projectRoot 'tools\go\bin\go.exe'
if(!(Test-Path $go)){$go=(Get-Command go -ErrorAction Stop).Source}
Push-Location $module
try{
 & $go test ./internal/...
 if($LASTEXITCODE){throw 'Cable tests failed'}
 & $go test ./cmd/kmx6cables
 if($LASTEXITCODE){throw 'Mix bridge reconnection tests failed'}
 & $go build -trimpath -ldflags '-H=windowsgui' -o $hostOutput ./cmd/kmx6cables
 if($LASTEXITCODE){throw 'Cable host build failed'}
}finally{Pop-Location}

