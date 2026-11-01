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

type mixSession struct { connection net.Conn; cancel context.CancelFunc; done chan struct{} }
type mixOwner struct { sync.Mutex; current *mixSession }
// Serialize replacement so an old producer cannot write after a new one starts.
// Cancellation also wakes a producer parked on a full USB capture buffer.
func (o *mixOwner) replace(parent context.Context,c net.Conn)(context.Context,*mixSession){
 o.Lock();defer o.Unlock()
 if old:=o.current;old!=nil {old.cancel();old.connection.Close();<-old.done}
 ctx,cancel:=context.WithCancel(parent);session:=&mixSession{c,cancel,make(chan struct{})};o.current=session;return ctx,session
}
func serveMixes(ctx context.Context,devices []*uac1.Device,logger *log.Logger)error{
 listener,err:=net.Listen("tcp","127.0.0.1:3242");if err!=nil{return err};return serveMixesListener(ctx,devices,logger,listener)
}
func serveMixesListener(parent context.Context,devices []*uac1.Device,logger *log.Logger,listener net.Listener)error{
 ctx,cancel:=context.WithCancel(parent);var workers sync.WaitGroup
 defer workers.Wait();defer cancel()
 go func(){<-ctx.Done();listener.Close()}()
 var owners [4]mixOwner
 for {
  connection,err:=listener.Accept();if err!=nil{if ctx.Err()!=nil{return nil};return err}
  workers.Add(1)
  go func(c net.Conn){
   defer workers.Done();releaseAudio:=usbip.BeginAudioScheduling();defer releaseAudio();defer c.Close()
   if tcp,ok:=c.(*net.TCPConn);ok{tcp.SetNoDelay(true);usbip.TuneAudioTCP(tcp)}
   c.SetDeadline(time.Now().Add(3*time.Second));var hello [5]byte
   if _,err:=io.ReadFull(c,hello[:]);err!=nil{return}
   if string(hello[:4])!="KMX1"||hello[4]>3{return};index:=int(hello[4])
   stream,session:=owners[index].replace(ctx,c);defer close(session.done);defer session.cancel()
   // Close blocked network I/O on shutdown or when a replacement takes ownership.
   finished:=make(chan struct{});defer close(finished)
   go func(){select{case <-stream.Done():c.Close();case <-finished:}}()
   device:=devices[index+2];device.Buffer.Reset();pcm:=make([]byte,240*4)
   ticker:=time.NewTicker(20*time.Millisecond);defer ticker.Stop()
   for stream.Err()==nil {
    if device.CaptureActive()&&device.Buffer.Available()>=240*4*4 {select{case <-stream.Done():return;case <-device.Buffer.Changed():case <-ticker.C:};continue}
    if !device.CaptureActive(){device.Buffer.Reset();select{case <-stream.Done():return;case <-time.After(5*time.Millisecond):}}
    c.SetDeadline(time.Now().Add(2*time.Second))
    if _,err:=c.Write([]byte{240,0,0,0});err!=nil{return}
    if _,err:=io.ReadFull(c,pcm);err!=nil{return}
    if stream.Err()!=nil{return}
    if device.CaptureActive(){device.Buffer.Write(pcm)}
   }
  }(connection)
 }
}
