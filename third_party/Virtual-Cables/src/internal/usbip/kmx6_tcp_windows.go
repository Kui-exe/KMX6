package usbip
import("net";"syscall";"unsafe")
var audioWSAIoctl=syscall.NewLazyDLL("ws2_32.dll").NewProc("WSAIoctl")
// This is per connection. It does not change Windows' global TCP policy.
// ACK every segment, avoiding a delayed-ACK/Nagle pause between USB/IP header,
// payload and isochronous metadata arriving in separate transport writes.
func TuneAudioTCP(tcp *net.TCPConn)error{
 raw,err:=tcp.SyscallConn();if err!=nil{return err};var optionErr error
 err=raw.Control(func(socket uintptr){frequency:=uint32(1);var returned uint32;result,_,callErr:=audioWSAIoctl.Call(socket,2550136855,uintptr(unsafe.Pointer(&frequency)),4,0,0,uintptr(unsafe.Pointer(&returned)),0,0);if int32(result)==-1{optionErr=callErr}})
 if err!=nil{return err};return optionErr
}