package audio

import "sync"

// Ring is a bounded, thread-safe byte ring buffer. Audio written by the
// playback endpoint is read by the capture endpoint of the same virtual cable.
type Ring struct {
    changed chan struct{}
	mu        sync.Mutex
	buf       []byte
	read      int
	write     int
	size      int
	dropped   uint64
	underruns uint64
}

func NewRing(capacity int) *Ring {
	if capacity < 1 {
		capacity = 1
	}
	return &Ring{buf: make([]byte, capacity),changed:make(chan struct{},1)}
}

func (r *Ring) Changed() <-chan struct{} {return r.changed}
func (r *Ring) notify(){select{case r.changed<-struct{}{}:default:}}
func (r *Ring) Capacity() int { return len(r.buf) }

func (r *Ring) Available() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.size
}

func (r *Ring) Stats() (available int, dropped uint64, underruns uint64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.size, r.dropped, r.underruns
}

// Write keeps the newest audio. If the ring is full, the oldest bytes are
// discarded so the stream can continue instead of blocking the USB worker.
func (r *Ring) Write(p []byte) int {
    defer r.notify()
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(p) >= len(r.buf) {
		keep := p[len(p)-len(r.buf):]
		r.dropped += uint64(r.size + len(p) - len(keep))
		copy(r.buf, keep)
		r.read, r.write, r.size = 0, 0, len(r.buf)
		return len(p)
	}
	overflow := len(p) - (len(r.buf) - r.size)
	if overflow > 0 {
		r.read = (r.read + overflow) % len(r.buf)
		r.size -= overflow
		r.dropped += uint64(overflow)
	}
	first := len(p)
	if first > len(r.buf)-r.write {
		first = len(r.buf) - r.write
	}
	copy(r.buf[r.write:r.write+first], p[:first])
	copy(r.buf[:len(p)-first], p[first:])
	r.write = (r.write + len(p)) % len(r.buf)
	r.size += len(p)
	return len(p)
}

// ReadSilence reads len(p) bytes. Missing audio is filled with digital silence
// (zero-valued PCM samples), which is preferable to stalling an isochronous URB.
func (r *Ring) ReadSilence(p []byte) int {
    defer r.notify()
	r.mu.Lock()
	defer r.mu.Unlock()
	n := len(p)
	if n > r.size {
		n = r.size
	}
	first := n
	if first > len(r.buf)-r.read {
		first = len(r.buf) - r.read
	}
	copy(p[:first], r.buf[r.read:r.read+first])
	copy(p[first:n], r.buf[:n-first])
	clear(p[n:])
	r.read = (r.read + n) % len(r.buf)
	r.size -= n
	if n < len(p) {
		r.underruns += uint64(len(p) - n)
	}
	return n
}
func (r *Ring) Reset() {
    defer r.notify()
	r.mu.Lock()
	defer r.mu.Unlock()
	r.read, r.write, r.size = 0, 0, 0
}
