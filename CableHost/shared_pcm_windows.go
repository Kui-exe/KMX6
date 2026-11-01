package usbip
import (
 "fmt"
 "sync/atomic"
 "syscall"
 "unsafe"
 "virtualcables/internal/uac1"
)
const pcmCapacity=65536
const pcmBytes=64+pcmCapacity*4
var kernelPCM=syscall.NewLazyDLL("kernel32.dll")
var pcmCreate=kernelPCM.NewProc("CreateFileMappingW")
var pcmMap=kernelPCM.NewProc("MapViewOfFile")
var pcmEvent=kernelPCM.NewProc("CreateEventW")
var pcmSignal=kernelPCM.NewProc("SetEvent")
var pcmUnmap=kernelPCM.NewProc("UnmapViewOfFile")
var pcmClose=kernelPCM.NewProc("CloseHandle")
type sharedPCM struct{mapping,event,address uintptr;memory []byte}
var sharedPlayback=map[*uac1.Device]*sharedPCM{}
func(s *sharedPCM)word(offset int)*uint32{return (*uint32)(unsafe.Pointer(&s.memory[offset]))}
func newSharedPCM(name string)(*sharedPCM,error){n,_:=syscall.UTF16PtrFromString(name);h,_,err:=pcmCreate.Call(^uintptr(0),0,4,0,pcmBytes,uintptr(unsafe.Pointer(n)));if h==0{return nil,fmt.Errorf("PCM mapping: %v",err)};address,_,err:=pcmMap.Call(h,0xf001f,0,0,pcmBytes);if address==0{pcmClose.Call(h);return nil,fmt.Errorf("PCM map view: %v",err)};eventName,_:=syscall.UTF16PtrFromString(name+"Ready");event,_,err:=pcmEvent.Call(0,0,0,uintptr(unsafe.Pointer(eventName)));if event==0{pcmUnmap.Call(address);pcmClose.Call(h);return nil,fmt.Errorf("PCM event: %v",err)};s:=&sharedPCM{h,event,address,unsafe.Slice((*byte)(unsafe.Pointer(address)),pcmBytes)};atomic.StoreUint32(s.word(0),0x324d504f);atomic.StoreUint32(s.word(4),0);atomic.StoreUint32(s.word(8),0);atomic.AddUint32(s.word(12),1);return s,nil}
func(s *sharedPCM)close(){pcmUnmap.Call(s.address);pcmClose.Call(s.event);pcmClose.Call(s.mapping)}
func(s *sharedPCM)write(data []byte){w:=atomic.LoadUint32(s.word(4));r:=atomic.LoadUint32(s.word(8));available:=w-r;if available>=pcmCapacity{return};frames:=uint32(len(data)/4);if frames>pcmCapacity-1-available{frames=pcmCapacity-1-available};for offset:=uint32(0);offset<frames;{index:=(w+offset)&(pcmCapacity-1);count:=frames-offset;if count>pcmCapacity-index{count=pcmCapacity-index};copy(s.memory[64+index*4:64+(index+count)*4],data[offset*4:(offset+count)*4]);offset+=count};atomic.StoreUint32(s.word(4),w+frames);pcmSignal.Call(s.event)}
func InitSharedPlayback(devices []*uac1.Device)error{for i:=0;i<2;i++{s,err:=newSharedPCM(fmt.Sprintf("Local\\KMX6PCM%d",i+1));if err!=nil{return err};sharedPlayback[devices[i]]=s};return nil}
func CloseSharedPlayback(){for _,s:=range sharedPlayback{s.close()}}
func publishSharedPlayback(device *uac1.Device,data []byte){if s:=sharedPlayback[device];s!=nil{s.write(data)}}