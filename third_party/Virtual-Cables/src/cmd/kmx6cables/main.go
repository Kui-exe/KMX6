// KMX6 endpoint host. The USB audio/USB-IP implementation is the
// BSD-2-Clause Virtual-Cables project by Tarek Wasfy and AI (see third_party).
package main

import (
 "context"
 "encoding/binary"
 "flag"
 "fmt"
 "log"
 "os"
 "os/signal"
 "path/filepath"
 "time"
 "unicode/utf16"
 "virtualcables/internal/uac1"
 "virtualcables/internal/usbip"
)

func usbString(s string) []byte {
 units:=utf16.Encode([]rune(s)); b:=make([]byte,2+2*len(units));b[0]=byte(len(b));b[1]=3
 for i,v:=range units {binary.LittleEndian.PutUint16(b[2+i*2:],v)}
 return b
}
func devices() ([]*uac1.Device,error) {
 names:=[]string{"KMX6 Computer 1","KMX6 Computer 2","KMX6 Mic B1","KMX6 Mic B2","KMX6 Mic B3","KMX6 Mic B4"}
 var result []*uac1.Device
 for i,name:=range names {
  // Capacity, not a prefill delay. Windows can queue several 10 ms USB
  // transfers ahead; a 20 ms capacity dropped valid queued audio.
  d,err:=uac1.NewDevice(i+1,100);if err!=nil{return nil,err}
  binary.LittleEndian.PutUint16(d.Descriptors.Device[10:12],uint16(0xCB01+i))
  d.Descriptors.Product=name;d.Descriptors.Strings[2]=usbString(name)
  d.Descriptors.Serial=fmt.Sprintf("KMX6%03d",i+1);d.Descriptors.Strings[3]=usbString(d.Descriptors.Serial)
  d.Descriptors.Strings[1]=usbString("KMX6")
  if err=singleDirection(d,i>=2);err!=nil{return nil,err}
  if err=d.Descriptors.Validate();err!=nil{return nil,err};result=append(result,d)
 }
 return result,nil
}
func main(){
 test:=flag.Bool("self-test",false,"validate endpoint descriptors and PCM transport")
 flag.Parse()
 executable,_:=os.Executable();folder:=filepath.Dir(executable)
 ds,err:=devices();if err!=nil{log.Fatal(err)}
 if *test {
  for _,d:=range ds {
   d.HandleControl(uac1.SetupPacket{RequestType:0,Request:uac1.RequestSetConfiguration,Value:1},nil)
   d.HandleControl(uac1.SetupPacket{RequestType:1,Request:uac1.RequestSetInterface,Index:1,Value:1},nil)
   if d.CaptureOnly {if !d.CaptureActive()||d.PlaybackActive(){log.Fatal("Invalid capture direction")}}else{if !d.PlaybackActive()||d.CaptureActive(){log.Fatal("Invalid playback direction")}}
  }
  os.WriteFile(filepath.Join(folder,"cable-self-test.txt"),[]byte("PASS: exactly two playback-only and four capture-only endpoint descriptors\n"),0644)
  return
 }
 if err=usbip.InitSharedPlayback(ds);err!=nil{log.Fatal(err)};defer usbip.CloseSharedPlayback()
 file,err:=os.OpenFile(filepath.Join(folder,"cable-host.log"),os.O_CREATE|os.O_APPEND|os.O_WRONLY,0644);if err!=nil{log.Fatal(err)};defer file.Close()
 logger:=log.New(file,"",log.LstdFlags)
 ctx,cancel:=signal.NotifyContext(context.Background(),os.Interrupt);defer cancel()
 go func(){if err:=serveMixes(ctx,ds,logger);err!=nil{logger.Printf("Mix bridge: %v",err);cancel()}}()
 go func(){if err:=servePlayback(ctx,ds,logger);err!=nil{logger.Printf("Playback bridge: %v",err);cancel()}}()
 server:=usbip.NewServer("127.0.0.1:3241",ds,logger)
 go func(){ticker:=time.NewTicker(10*time.Second);defer ticker.Stop();for{select{case <-ctx.Done():return;case <-ticker.C:for _,d:=range ds{queued,dropped,under:=d.Buffer.Stats();logger.Printf("%s queued=%d droppedBytes=%d silenceBytes=%d",d.Product(),queued,dropped,under)}}}}()
 if err=server.Serve(ctx);err!=nil{logger.Fatal(err)}
}
