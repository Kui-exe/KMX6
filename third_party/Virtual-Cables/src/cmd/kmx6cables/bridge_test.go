package main
import (
 "context"
 "encoding/binary"
 "fmt"
 "io"
 "log"
 "net"
 "testing"
 "time"
 "virtualcables/internal/uac1"
)
func TestMixerReconnectReplacesParkedCaptureSessions(t *testing.T){
 ds,err:=devices();if err!=nil{t.Fatal(err)}
 for _,d:=range ds[2:] {d.HandleControl(uac1.SetupPacket{RequestType:0,Request:uac1.RequestSetConfiguration,Value:1},nil);d.HandleControl(uac1.SetupPacket{RequestType:1,Request:uac1.RequestSetInterface,Index:1,Value:1},nil)}
 listener,err:=net.Listen("tcp","127.0.0.1:0");if err!=nil{t.Fatal(err)}
 ctx,cancel:=context.WithCancel(context.Background());done:=make(chan error,1)
 go func(){done<-serveMixesListener(ctx,ds,log.New(io.Discard,"",0),listener)}()
 defer func(){cancel();select{case err:=<-done:if err!=nil{t.Error(err)};case <-time.After(3*time.Second):t.Error("capture sessions did not stop")}}()
 dial:=func(t *testing.T,bus int)net.Conn{c,err:=net.DialTimeout("tcp",listener.Addr().String(),time.Second);if err!=nil{t.Fatal(err)};c.SetDeadline(time.Now().Add(time.Second));if _,err=c.Write(append([]byte("KMX1"),byte(bus)));err!=nil{t.Fatal(err)};return c}
 readBlock:=func(t *testing.T,c net.Conn){var header [4]byte;if _,err:=io.ReadFull(c,header[:]);err!=nil{t.Fatal(err)};if binary.LittleEndian.Uint32(header[:])!=240{t.Fatalf("bad audio request %v",header)};if _,err:=c.Write(make([]byte,960));err!=nil{t.Fatal(err)}}
 for bus:=0;bus<4;bus++ {t.Run(fmt.Sprintf("B%d",bus+1),func(t *testing.T){
  old:=dial(t,bus);for i:=0;i<4;i++{readBlock(t,old)}
  deadline:=time.Now().Add(time.Second);for ds[bus+2].Buffer.Available()<3840&&time.Now().Before(deadline){time.Sleep(time.Millisecond)}
  if ds[bus+2].Buffer.Available()<3840{t.Fatal("old producer never reached full capture buffer")}
  replacement:=dial(t,bus);readBlock(t,replacement);replacement.Close();old.Close()
  // A capture stop must not leave another parked owner behind.
  ds[bus+2].HandleControl(uac1.SetupPacket{RequestType:1,Request:uac1.RequestSetInterface,Index:1,Value:0},nil)
  reopened:=dial(t,bus);readBlock(t,reopened);reopened.Close()
 })}
}
