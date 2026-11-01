package main

import (
 "encoding/binary"
 "virtualcables/internal/uac1"
)

// Exactly one audio direction per USB device. The application reads virtual
// playback PCM directly over local IPC and feeds capture-only mixes over local IPC.
func singleDirection(d *uac1.Device, capture bool) error {
 d.CaptureOnly=capture;d.PlaybackOnly=!capture
 var b []byte
 add:=func(v ...byte){b=append(b,v...)}
 add(9,2,0,0,2,1,0,0x80,50)
 add(9,4,0,0,0,1,1,0,0)
 add(9,0x24,1,0,1,40,0,1,1)
 terminal:=byte(1);endpoint:=byte(1);attributes:=byte(9)
 if capture {
  add(12,0x24,2,4,1,2,0,2,3,0,0,0)
  add(10,0x24,6,5,4,1,3,0,0,0)
  add(9,0x24,3,6,1,1,0,5,0)
  terminal=6;endpoint=0x82;attributes=0x0d
 }else{
  add(12,0x24,2,1,1,1,0,2,3,0,0,0)
  add(10,0x24,6,2,1,1,3,0,0,0)
  add(9,0x24,3,3,1,3,0,2,0)
 }
 add(9,4,1,0,0,1,2,0,0)
 add(9,4,1,1,1,1,2,0,0)
 add(7,0x24,1,terminal,1,1,0)
 add(11,0x24,2,1,2,2,16,1,0x80,0xbb,0)
 add(9,5,endpoint,attributes,0xc0,0,1,0,0)
 add(7,0x25,1,0,0,0,0)
 binary.LittleEndian.PutUint16(b[2:4],uint16(len(b)))
 d.Descriptors.Config=b
 return d.Descriptors.Validate()
}
