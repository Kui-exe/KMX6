package main

import (
 "context"
 "io"
 "log"
 "net"
 "sync"
 "time"
 "virtualcables/internal/uac1"
 "virtualcables/internal/usbip"
)

// Stream the PCM already delivered by Windows to each playback endpoint.
// This removes the extra WASAPI loopback graph and its capture buffer.
func servePlayback(ctx context.Context,devices []*uac1.Device,logger *log.Logger) error {
 listener,err:=net.Listen("tcp","127.0.0.1:3243");if err!=nil{return err}
 go func(){<-ctx.Done();listener.Close()}()
 var owners [2]sync.Mutex
 for {connection,err:=listener.Accept();if err!=nil{if ctx.Err()!=nil{return nil};return err}
  go func(c net.Conn){defer c.Close();if tcp,ok:=c.(*net.TCPConn);ok{tcp.SetNoDelay(true);tcp.SetWriteBuffer(65536)};c.SetDeadline(time.Now().Add(2*time.Second));var hello [5]byte;if _,err:=io.ReadFull(c,hello[:]);err!=nil{return};if string(hello[:4])!="KMXC"||hello[4]>1{return};index:=int(hello[4]);if !owners[index].TryLock(){return};defer owners[index].Unlock();release:=usbip.BeginAudioScheduling();defer release();device:=devices[index];device.Buffer.Reset();pcm:=make([]byte,240*4)
   readerCtx,cancel:=context.WithCancel(ctx);defer cancel()
   c.SetReadDeadline(time.Time{})
   go func(){var probe [1]byte;c.Read(probe[:]);cancel();c.Close()}()
   wait:=func()bool{select{case <-readerCtx.Done():return false;case <-device.Buffer.Changed():return true}}
   for readerCtx.Err()==nil {
    if !device.PlaybackActive(){device.Buffer.Reset();if !wait(){return};continue}
    if device.Buffer.Available()<len(pcm){if !wait(){return};continue}
    device.Buffer.ReadSilence(pcm);c.SetWriteDeadline(time.Now().Add(2*time.Second))
    for offset:=0;offset<len(pcm);{n,err:=c.Write(pcm[offset:]);if err!=nil||n==0{return};offset+=n}
   }  }(connection)
 }
}