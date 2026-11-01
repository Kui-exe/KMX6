package usbip

import (
 "context"
 "virtualcables/internal/uac1"
 "io"
 "testing"
 "time"
)

func TestKMX6AudioPacketsKeepSubmissionOrder(t *testing.T){
 state:=newConnectionState(io.Discard)
 ctx,cancel:=context.WithCancel(context.Background());defer cancel()
 var observed []uint32
 for i:=uint32(0);i<32;i++{
  var submit parsedSubmit;submit.req.Basic.Endpoint=1;submit.req.Basic.Sequence=i
  jobContext,jobCancel:=context.WithCancel(ctx);state.wg.Add(1)
  state.enqueueKMX6Iso(ctx,jobContext,jobCancel,submit,func(_ context.Context,job parsedSubmit)error{
   if job.req.Basic.Sequence==0{time.Sleep(5*time.Millisecond)}
   observed=append(observed,job.req.Basic.Sequence);return nil
  })
 }
 state.wg.Wait()
 if len(observed)!=32{t.Fatalf("processed %d packets",len(observed))}
 for i,sequence:=range observed{if sequence!=uint32(i){t.Fatalf("packet %d arrived at position %d",sequence,i)}}
}

func TestKMX6JitterDoesNotAccumulateIntoClockDrift(t *testing.T) {
 state:=newConnectionState(io.Discard)
 start:=time.Unix(100,0)
 deadline:=state.reserveIsoCompletion(1,10,start)
 for i:=0;i<1000;i++ {
  // Simulate each URB submission arriving 1 ms after its nominal predecessor.
  // Resetting the timeline here makes a 48 kHz endpoint run at ~43.6 kHz.
  deadline=state.reserveIsoCompletion(1,10,deadline.Add(time.Millisecond))
 }
 if want:=start.Add(10010*time.Millisecond);!deadline.Equal(want){t.Fatalf("clock drifted: %v, want %v",deadline,want)}
 // A long pause is a restart, not a request to burst seconds of stale audio.
 resumed:=state.reserveIsoCompletion(1,10,deadline.Add(time.Second))
 if want:=deadline.Add(1010*time.Millisecond);!resumed.Equal(want){t.Fatalf("restart deadline %v, want %v",resumed,want)}
 state.resetIsoTimeline(1)
 reopened:=state.reserveIsoCompletion(1,10,start)
 if want:=start.Add(10*time.Millisecond);!reopened.Equal(want){t.Fatalf("reopened stream inherited stale timeline: %v",reopened)}
}

func TestKMX6PCMIsAvailableBeforeUSBCompletion(t *testing.T){
 device,err:=uac1.NewDevice(1,100);if err!=nil{t.Fatal(err)};device.PlaybackOnly=true
 device.HandleControl(uac1.SetupPacket{RequestType:0,Request:uac1.RequestSetConfiguration,Value:1},nil)
 device.HandleControl(uac1.SetupPacket{RequestType:1,Request:uac1.RequestSetInterface,Index:1,Value:1},nil)
 var submit parsedSubmit;submit.req.Basic.Endpoint=1;submit.req.Basic.Direction=DirectionOut;submit.req.NumberOfPackets=10;submit.completeAt=time.Now().Add(time.Hour);submit.out=[]byte{1,2,3,4}
 stageKMX6Playback(device,&submit)
 if !submit.playbackStaged||device.Buffer.Available()!=4{t.Fatal("PCM was held behind USB completion")}
 stageKMX6Playback(device,&submit)
 if device.Buffer.Available()!=4{t.Fatal("PCM staged twice")}
}
