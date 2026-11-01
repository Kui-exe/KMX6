// Windows audio scheduling for the USB endpoint workers. All calls run outside
// the mixer callback. Failure leaves normal scheduling in place.
package usbip

import (
 "runtime"
 "syscall"
 "unsafe"
)

func BeginAudioScheduling() func() {
 runtime.LockOSThread()
 dll := syscall.NewLazyDLL("avrt.dll")
 begin := dll.NewProc("AvSetMmThreadCharacteristicsW")
 priority := dll.NewProc("AvSetMmThreadPriority")
 end := dll.NewProc("AvRevertMmThreadCharacteristics")
 if begin.Find()!=nil || priority.Find()!=nil || end.Find()!=nil { runtime.UnlockOSThread();return func(){} }
 name,_ := syscall.UTF16PtrFromString("Pro Audio")
 var index uint32
 handle,_,_ := begin.Call(uintptr(unsafe.Pointer(name)),uintptr(unsafe.Pointer(&index)))
 if handle==0 {runtime.UnlockOSThread();return func(){}}
 priority.Call(handle,1)
 return func(){end.Call(handle);runtime.UnlockOSThread()}
}