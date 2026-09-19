//go:build windows

// Package api aggregates data from RTSS and MSI Afterburner shared memory
// (plus direct Windows queries for system RAM) into one Metrics snapshot,
// and serves it as JSON and as a compact fixed-size binary blob intended
// for frequent polling from resource-constrained clients (e.g. an ESP32).
package api

import (
	"net/http"
	"strconv"
	"sync/atomic"
	"time"

	"rtss_api/internal/mahm"
	"rtss_api/internal/rtss"
	"rtss_api/internal/sysmem"
)

// Server holds the shared-memory readers and serves the HTTP API.
type Server struct {
	rtss       *rtss.Reader
	mahm       *mahm.Reader
	defaultGPU uint32
	seq        uint32
}

// NewServer builds a Server. defaultGPU selects which Afterburner GPU
// index (0-based) is reported when a request does not specify ?gpu=.
func NewServer(defaultGPU uint32) *Server {
	return &Server{
		rtss:       rtss.NewReader(),
		mahm:       mahm.NewReader(),
		defaultGPU: defaultGPU,
	}
}

// Close releases the underlying shared memory mappings.
func (s *Server) Close() {
	s.rtss.Close()
	s.mahm.Close()
}

// Metrics is the full JSON response shape.
type Metrics struct {
	TimestampUnixMS int64   `json:"ts"`
	Sources         Sources `json:"sources"`
	GPU             GPU     `json:"gpu"`
	CPU             CPU     `json:"cpu"`
	Mem             Mem     `json:"mem"`
	FPS             FPS     `json:"fps"`
}

type Sources struct {
	RTSS        bool `json:"rtss"`
	Afterburner bool `json:"afterburner"`
}

type GPU struct {
	TemperatureC *float64 `json:"temperature_c"`
	LoadPercent  *float64 `json:"load_percent"`
	CoreClockMHz *float64 `json:"core_clock_mhz"`
	PowerWatts   *float64 `json:"power_watts"`
	MemClockMHz  *float64 `json:"mem_clock_mhz"`
	MemTotalMB   *uint64  `json:"mem_total_mb"`
	MemUsedMB    *uint64  `json:"mem_used_mb"`
}

type CPU struct {
	TemperatureC *float64 `json:"temperature_c"`
	LoadPercent  *float64 `json:"load_percent"`
	PowerWatts   *float64 `json:"power_watts"`
}

type Mem struct {
	UsedMB  *uint64 `json:"used_mb"`
	TotalMB *uint64 `json:"total_mb"`
	FreqMHz *uint32 `json:"freq_mhz"`
}

type FPS struct {
	Process string   `json:"process"`
	PID     uint32   `json:"pid"`
	Current *float64 `json:"current"`
	Avg     *float64 `json:"avg"`
	Min     *float64 `json:"min"`
	Max     *float64 `json:"max"`
}

func f64p(v float64) *float64 { return &v }
func u64p(v uint64) *uint64   { return &v }
func u32p(v uint32) *uint32   { return &v }

func entryPtr(snap mahm.Snapshot, srcID, gpu uint32) *float64 {
	e, ok := snap.Get(srcID, gpu)
	if !ok || !e.OK {
		return nil
	}
	return f64p(float64(e.Value))
}

// selectorFromQuery parses ?process= and ?pid= into a rtss.Selector, and
// ?gpu= into a GPU index (falling back to the server default).
func (s *Server) selectorFromQuery(r *http.Request) (rtss.Selector, uint32) {
	q := r.URL.Query()

	sel := rtss.Selector{ProcessName: q.Get("process")}
	if pidStr := q.Get("pid"); pidStr != "" {
		if pid, err := strconv.ParseUint(pidStr, 10, 32); err == nil {
			sel.PID = uint32(pid)
		}
	}

	gpu := s.defaultGPU
	if gpuStr := q.Get("gpu"); gpuStr != "" {
		if g, err := strconv.ParseUint(gpuStr, 10, 32); err == nil {
			gpu = uint32(g)
		}
	}
	return sel, gpu
}

// Collect gathers one consistent snapshot from all sources.
func (s *Server) Collect(sel rtss.Selector, gpuIdx uint32) Metrics {
	rtssStats := s.rtss.Snapshot(sel)
	hwSnap := s.mahm.Read()

	m := Metrics{
		TimestampUnixMS: time.Now().UnixMilli(),
		Sources: Sources{
			RTSS:        rtssStats.Available,
			Afterburner: hwSnap.Available,
		},
	}

	if hwSnap.Available {
		m.GPU = GPU{
			TemperatureC: entryPtr(hwSnap, mahm.SrcGPUTemperature, gpuIdx),
			LoadPercent:  entryPtr(hwSnap, mahm.SrcGPUUsage, gpuIdx),
			CoreClockMHz: entryPtr(hwSnap, mahm.SrcCoreClock, gpuIdx),
			PowerWatts:   entryPtr(hwSnap, mahm.SrcGPUAbsPower, gpuIdx),
			MemClockMHz:  entryPtr(hwSnap, mahm.SrcMemoryClock, gpuIdx),
		}
		if total, ok := hwSnap.GPUMemTotalMB(gpuIdx); ok {
			m.GPU.MemTotalMB = u64p(uint64(total))
		}
		if e, ok := hwSnap.Get(mahm.SrcGPUMemoryUsage, gpuIdx); ok && e.OK {
			m.GPU.MemUsedMB = u64p(uint64(e.Value))
		}
		m.CPU = CPU{
			TemperatureC: entryPtr(hwSnap, mahm.SrcCPUTemperature, 0xFFFFFFFF),
			LoadPercent:  entryPtr(hwSnap, mahm.SrcCPUUsage, 0xFFFFFFFF),
			PowerWatts:   entryPtr(hwSnap, mahm.SrcCPUPower, 0xFFFFFFFF),
		}
	}

	if usage, ok := sysmem.ReadUsage(); ok {
		m.Mem.UsedMB = u64p(usage.UsedMB)
		m.Mem.TotalMB = u64p(usage.TotalMB)
	}
	if f := sysmem.RAMSpeedMHz(); f > 0 {
		m.Mem.FreqMHz = u32p(f)
	}

	if rtssStats.Available {
		m.FPS = FPS{
			Process: rtssStats.ProcessName,
			PID:     rtssStats.ProcessID,
			Current: f64p(rtssStats.FPSCurrent),
			Avg:     f64p(rtssStats.FPSAvg),
			Min:     f64p(rtssStats.FPSMin),
			Max:     f64p(rtssStats.FPSMax),
		}
	}

	atomic.AddUint32(&s.seq, 1)
	return m
}
