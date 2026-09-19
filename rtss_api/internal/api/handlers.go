//go:build windows

package api

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"sync/atomic"
)

// Routes registers all HTTP handlers on mux.
func (s *Server) Routes(mux *http.ServeMux) {
	mux.HandleFunc("/api/v1/health", s.handleHealth)
	mux.HandleFunc("/api/v1/metrics", s.handleMetricsJSON)
	mux.HandleFunc("/api/v1/metrics.bin", s.handleMetricsBinary)
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	sel, gpu := s.selectorFromQuery(r)
	m := s.Collect(sel, gpu)

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"status":      "ok",
		"rtss":        m.Sources.RTSS,
		"afterburner": m.Sources.Afterburner,
	})
}

func (s *Server) handleMetricsJSON(w http.ResponseWriter, r *http.Request) {
	sel, gpu := s.selectorFromQuery(r)
	m := s.Collect(sel, gpu)

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(m)
}

// binaryMagic identifies the .bin payload format for clients (ASCII
// "RTAB" = "RTSS/Afterburner API").
const binaryMagic uint32 = 0x42415452 // little-endian bytes: 'R','T','A','B'

// naFloat32 is the "value not available" sentinel used in the binary
// payload. Every metric in this API is physically non-negative, so a
// negative sentinel is unambiguous and cheap to check in C:
// `if (v < 0) { /* not available */ }`.
const naFloat32 float32 = -1

func f32or(p *float64, def float32) float32 {
	if p == nil {
		return def
	}
	return float32(*p)
}

func u32or(p *uint64, def uint32) uint32 {
	if p == nil {
		return def
	}
	return uint32(*p)
}

func u32orU32(p *uint32, def uint32) uint32 {
	if p == nil {
		return def
	}
	return *p
}

// handleMetricsBinary serves a fixed 72-byte little-endian payload meant
// for high-frequency polling from a microcontroller. See README.md
// "Binary polling endpoint" for the exact field layout and a C struct
// matching it byte for byte.
func (s *Server) handleMetricsBinary(w http.ResponseWriter, r *http.Request) {
	sel, gpu := s.selectorFromQuery(r)
	m := s.Collect(sel, gpu)

	buf := new(bytes.Buffer)
	buf.Grow(72)

	_ = binary.Write(buf, binary.LittleEndian, binaryMagic)
	_ = binary.Write(buf, binary.LittleEndian, atomic.LoadUint32(&s.seq))

	_ = binary.Write(buf, binary.LittleEndian, f32or(m.GPU.TemperatureC, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, f32or(m.GPU.LoadPercent, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, f32or(m.GPU.CoreClockMHz, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, f32or(m.GPU.PowerWatts, naFloat32))

	_ = binary.Write(buf, binary.LittleEndian, f32or(m.CPU.TemperatureC, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, f32or(m.CPU.LoadPercent, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, f32or(m.CPU.PowerWatts, naFloat32))

	_ = binary.Write(buf, binary.LittleEndian, u32or(m.Mem.UsedMB, 0))
	_ = binary.Write(buf, binary.LittleEndian, u32or(m.Mem.TotalMB, 0))
	_ = binary.Write(buf, binary.LittleEndian, u32orU32(m.Mem.FreqMHz, 0))

	_ = binary.Write(buf, binary.LittleEndian, f32or(m.FPS.Current, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, f32or(m.FPS.Avg, naFloat32))

	_ = binary.Write(buf, binary.LittleEndian, m.FPS.PID)

	_ = binary.Write(buf, binary.LittleEndian, f32or(m.GPU.MemClockMHz, naFloat32))
	_ = binary.Write(buf, binary.LittleEndian, u32or(m.GPU.MemTotalMB, 0))
	_ = binary.Write(buf, binary.LittleEndian, u32or(m.GPU.MemUsedMB, 0))

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", "72")
	_, _ = w.Write(buf.Bytes())
}
