//go:build windows

// Package rtss reads the RivaTuner Statistics Server "RTSSSharedMemoryV2"
// shared memory section to obtain per-process framerate/frametime
// statistics.
//
// Layout constants below are transcribed from the official SDK header
// shipped with RTSS ("RTSS SDK\Include\RTSSSharedMemory.h", v2.20 layout,
// struct RTSS_SHARED_MEMORY / RTSS_SHARED_MEMORY_APP_ENTRY). Only fields
// declared before the ones we read have fixed offsets across RTSS
// versions -- the SDK only ever appends fields, it never reorders or
// removes them -- so these offsets are stable even against older RTSS
// builds.
package rtss

import (
	"strings"
	"sync"

	"rtss_api/internal/winshm"
)

const mappingName = `RTSSSharedMemoryV2`

// Top-level RTSS_SHARED_MEMORY header offsets.
const (
	hdrOffSignature                  = 0
	hdrOffVersion                    = 4
	hdrOffAppEntrySize               = 8  // dwAppEntrySize: live sizeof(RTSS_SHARED_MEMORY_APP_ENTRY)
	hdrOffAppArrOffset               = 12 // dwAppArrOffset: live offset of arrApp
	hdrOffLastForegroundApp          = 64
	hdrOffLastForegroundAppProcessID = 68
)

// signatureRTSS is the value of dwSignature when the shared memory holds
// valid data. MSVC evaluates the multichar constant 'RTSS' with the first
// character in the most significant byte: ('R'<<24)|('T'<<16)|('S'<<8)|'S'.
// Confirmed empirically by dumping the live section on this machine.
const signatureRTSS = 0x52545353

// Layout of RTSS_SHARED_MEMORY itself, up to and including
// qwLatencyMarkerResetTimestamp (v2.19+); this is where arrOSD begins.
const headerSize = 96

// RTSS_SHARED_MEMORY_OSD_ENTRY total size (v2.20): szOSD[256] +
// szOSDOwner[256] + szOSDEx[4096] + buffer[262144] + szOSDEx2[32768].
const osdEntrySize = 256 + 256 + 4096 + 262144 + 32768
const osdEntryCount = 8

// arrApp begins immediately after arrOSD[8]. Used only as a fallback if the
// live dwAppArrOffset header field (see Snapshot) looks implausible.
const appArrOffsetFallback = headerSize + osdEntrySize*osdEntryCount

// sizeof(RTSS_SHARED_MEMORY_APP_ENTRY) computed field-by-field from the
// v2.20 header (includes the trailing arrPerfCounters[256], added in
// v2.18). This grows whenever RTSS adds fields to the struct (e.g. v2.21
// added GPU/latency-marker timing fields, bumping the real size to 12416),
// so it is only a fallback -- the live dwAppEntrySize header field is
// preferred whenever it looks sane.
const appEntrySizeFallback = 12304
const appEntryCount = 256

const maxPath = 260

// Offsets within one RTSS_SHARED_MEMORY_APP_ENTRY.
const (
	appOffProcessID        = 0
	appOffName             = 4 // char[MAX_PATH]
	appOffFrameTime        = 280
	appOffStatFramerateMin = 304
	appOffStatFramerateAvg = 308
	appOffStatFramerateMax = 312
)

// Stats is a snapshot of one monitored process' framerate statistics.
type Stats struct {
	Available   bool
	ProcessName string
	ProcessID   uint32
	FrameTimeUS uint32  // current frame time, microseconds
	FPSCurrent  float64 // 1e6 / FrameTimeUS
	FPSAvg      float64
	FPSMin      float64
	FPSMax      float64
}

// Reader keeps a lazily-opened, reconnecting handle to the RTSS shared
// memory section.
type Reader struct {
	mu sync.Mutex
	m  *winshm.Mapping
}

func NewReader() *Reader { return &Reader{} }

// Close releases the underlying mapping, if open.
func (r *Reader) Close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.m != nil {
		r.m.Close()
		r.m = nil
	}
}

// ensure opens the mapping if it is not already open, or reopens it if a
// previous read found it invalid (e.g. RTSS was restarted).
func (r *Reader) ensure() (*winshm.Mapping, bool) {
	if r.m != nil {
		return r.m, true
	}
	m, err := winshm.OpenMapping(mappingName)
	if err != nil {
		return nil, false
	}
	if m.U32(hdrOffSignature) != signatureRTSS {
		m.Close()
		return nil, false
	}
	r.m = m
	return r.m, true
}

// Available reports whether RTSS's shared memory is currently reachable.
func (r *Reader) Available() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	_, ok := r.ensure()
	return ok
}

// Selector picks which monitored application's stats to return.
type Selector struct {
	ProcessName string // case-insensitive exact match on the executable name, e.g. "cyberpunk2077.exe"
	PID         uint32 // if non-zero, matched by process ID instead
}

// Snapshot reads current framerate statistics for the process selected by
// sel. If sel is zero-valued, the last-foreground application reported by
// RTSS is used; if that is unavailable, the most recently updated
// non-empty application slot is used instead.
func (r *Reader) Snapshot(sel Selector) Stats {
	r.mu.Lock()
	defer r.mu.Unlock()

	m, ok := r.ensure()
	if !ok {
		return Stats{}
	}
	if m.U32(hdrOffSignature) != signatureRTSS {
		// Producer likely restarted; drop and retry on next call.
		m.Close()
		r.m = nil
		return Stats{}
	}

	// Prefer the live stride/offset RTSS reports in its header -- the SDK
	// grows RTSS_SHARED_MEMORY_APP_ENTRY across versions (e.g. v2.21 added
	// GPU/latency-marker timing fields), so a hardcoded stride silently
	// misaligns every read past the first array slot once RTSS updates.
	// Fall back to the computed constants only if the live values look
	// implausible (e.g. a corrupt/uninitialized section).
	stride := uintptr(m.U32(hdrOffAppEntrySize))
	if stride < appEntrySizeFallback || stride > 1<<20 {
		stride = appEntrySizeFallback
	}

	arrOffset := uintptr(m.U32(hdrOffAppArrOffset))
	if arrOffset < headerSize || arrOffset > 1<<24 {
		arrOffset = appArrOffsetFallback
	}

	wantPID := sel.PID
	wantName := strings.ToLower(strings.TrimSpace(sel.ProcessName))

	if wantPID == 0 && wantName == "" {
		wantPID = m.U32(hdrOffLastForegroundAppProcessID)
	}

	bestIdx := -1

	for i := 0; i < appEntryCount; i++ {
		base := arrOffset + uintptr(i)*stride
		pid := m.U32(base + appOffProcessID)
		if pid == 0 {
			continue
		}

		if wantPID != 0 {
			if pid == wantPID {
				bestIdx = i
				break
			}
			continue
		}
		if wantName != "" {
			name := strings.ToLower(m.CString(base+appOffName, maxPath))
			if name == wantName {
				bestIdx = i
				break
			}
			continue
		}

		// No selector resolved (foreground PID unknown/stale): fall back
		// to the last slot that is actively rendering (non-zero frame
		// time), or the first non-empty slot if none are.
		ft := m.U32(base + appOffFrameTime)
		if bestIdx == -1 || ft != 0 {
			bestIdx = i
		}
	}

	if bestIdx == -1 {
		return Stats{}
	}

	base := arrOffset + uintptr(bestIdx)*stride
	frameTime := m.U32(base + appOffFrameTime)

	var fpsCurrent float64
	if frameTime > 0 {
		fpsCurrent = 1000000.0 / float64(frameTime)
	}

	// dwStatFramerateMin/Avg/Max and the 1%/0.1% low fields are stored by
	// RTSS as FPS * 10 (one implied decimal digit), confirmed empirically:
	// dwStatFrames/((dwStatTime1-dwStatTime0)/1000) * 10 == dwStatFramerateAvg
	// on a live sample. dwFrameTime (and thus FPSCurrent) is plain
	// microseconds and needs no such scaling.
	const statFixedPointScale = 10.0

	return Stats{
		Available:   true,
		ProcessName: m.CString(base+appOffName, maxPath),
		ProcessID:   m.U32(base + appOffProcessID),
		FrameTimeUS: frameTime,
		FPSCurrent:  fpsCurrent,
		FPSAvg:      float64(m.U32(base+appOffStatFramerateAvg)) / statFixedPointScale,
		FPSMin:      float64(m.U32(base+appOffStatFramerateMin)) / statFixedPointScale,
		FPSMax:      float64(m.U32(base+appOffStatFramerateMax)) / statFixedPointScale,
	}
}
