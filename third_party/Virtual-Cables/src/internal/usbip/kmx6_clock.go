package usbip

import (
 "context"
 "virtualcables/internal/uac1"
)

type kmx6IsoJob struct {
 ctx context.Context
 cancel context.CancelFunc
 submit parsedSubmit
 process func(context.Context,parsedSubmit) error
}

// USB packets on one endpoint are ordered. One goroutine per request can
// reorder PCM writes whenever the scheduler starts a later request first.
func(c *connectionState) enqueueKMX6Iso(parent,ctx context.Context,cancel context.CancelFunc,submit parsedSubmit,process func(context.Context,parsedSubmit)error){
 c.mu.Lock()
 if c.kmx6Workers==nil{c.kmx6Workers=make(map[uint32]chan kmx6IsoJob)}
 queue:=c.kmx6Workers[submit.req.Basic.Endpoint]
 if queue==nil{
  queue=make(chan kmx6IsoJob,256);c.kmx6Workers[submit.req.Basic.Endpoint]=queue
  go func(){
   releaseAudio := BeginAudioScheduling();defer releaseAudio()
   complete:=func(job kmx6IsoJob){defer c.wg.Done();defer job.cancel();defer c.removePending(job.submit.req.Basic.Sequence);_ = job.process(job.ctx,job.submit)}
   for{select{
   case job:=<-queue:complete(job)
   case <-parent.Done():
    for{select{case job:=<-queue:complete(job);default:return}}
   }}
  }()
 }
 c.mu.Unlock()
 job:=kmx6IsoJob{ctx,cancel,submit,process}
 select{case queue<-job:case <-parent.Done():cancel();c.removePending(submit.req.Basic.Sequence);c.wg.Done()}
}

// Stream restarts cancel queued USB transfers. Their reserved future deadlines
// must not delay the next stream or let playback outrun capture on the next open.
func (c *connectionState) resetIsoTimeline(endpoint uint32) {
 c.mu.Lock()
 delete(c.isoNext,endpoint)

 c.mu.Unlock()
}

// Windows queues several audio URBs ahead. Publish their PCM in receive order
// immediately; completion timers must not hold audio already in memory hostage.
func stageKMX6Playback(device *uac1.Device,submit *parsedSubmit){
 if submit.playbackStaged||!device.PlaybackOnly||submit.req.Basic.Endpoint!=1||submit.req.Basic.Direction!=DirectionOut||!submit.req.IsIsochronous(){return}
 if device.PlaybackActive(){device.WritePlayback(submit.out);publishSharedPlayback(device,submit.out);submit.playbackStaged=true}
}
func(c *connectionState) endpointContext(parent context.Context,endpoint uint32)context.Context{
 c.mu.Lock();defer c.mu.Unlock()
 if c.isoStreamContext==nil{c.isoStreamContext=make(map[uint32]context.Context);c.isoStreamCancel=make(map[uint32]context.CancelFunc)}
 if ctx:=c.isoStreamContext[endpoint];ctx!=nil{return ctx}
 ctx,cancel:=context.WithCancel(parent);c.isoStreamContext[endpoint]=ctx;c.isoStreamCancel[endpoint]=cancel;return ctx
}