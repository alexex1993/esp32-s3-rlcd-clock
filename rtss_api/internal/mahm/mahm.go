//go:build windows

// Package mahm reads MSI Afterburner's hardware monitoring shared memory
// section ("MAHMSharedMemory"). Afterburner (not RTSS) is the component
// that actually samples GPU/CPU sensors; RTSS only reuses this data for
// its own on-screen display. Afterburner must be running, and each sensor
// must be individually enabled ("checked") on the Monitoring tab of its
// settings -- entries that are not checked there simply do not appear in
// this shared memory array.
//
// Layout constants are transcribed from the official SDK header shipped
// with RTSS/Afterburner ("SDK\Include\MSIAB\MAHMSharedMemory.h").
package mahm

import (
	"strings"
	"sync"

	"rtss_api/internal/winshm"
)

const mappingName = `MAHMSharedMemory`
const mutexName = `Global\Access_MAHMSharedMemory`

// signatureMAHM is the value of dwSignature when the shared memory holds
// valid data (MSVC packs the multichar constant 'MAHM' with the first
// character in the most significant byte). Confirmed empirically by
// dumping the live section on this machine.
const signatureMAHM = 0x4D41484D

// MAHM_SHARED_MEMORY_HEADER offsets (v2.0 layout).
const (
	hdrOffSignature     = 0
	hdrOffVersion       = 4
	hdrOffHeaderSize    = 8
	hdrOffNumEntries    = 12
	hdrOffEntrySize     = 16
	hdrOffTime          = 20
	hdrOffNumGpuEntries = 24
	hdrOffGpuEntrySize  = 28
)

const headerSize = 32
const entrySizeFallback = 1324

const maxPath = 260

// Offsets within one MAHM_SHARED_MEMORY_ENTRY.
const (
	entryOffSrcName  = 0
	entryOffSrcUnits = maxPath     // 260
	entryOffData     = maxPath * 5 // szSrcName+szSrcUnits+szLocalizedSrcName+szLocalizedSrcUnits+szRecommendedFormat = 1300
	entryOffMinLimit = entryOffData + 4
	entryOffMaxLimit = entryOffData + 8
	entryOffFlags    = entryOffData + 12
	entryOffGpu      = entryOffData + 16
	entryOffSrcId    = entryOffData + 20
)

// Data source IDs, from MONITORING_SOURCE_ID_* in the SDK header.
const (
	SrcGPUTemperature = 0x00000000
	SrcCoreClock      = 0x00000020
	SrcMemoryClock    = 0x00000022
	SrcGPUUsage       = 0x00000030
	SrcGPUMemoryUsage = 0x00000031 // MONITORING_SOURCE_ID_MEMORY_USAGE -- Afterburner's "Memory usage" sensor; despite the generic name this is GPU video memory in use, in MB
	SrcGPURelPower    = 0x00000060
	SrcGPUAbsPower    = 0x00000061
	SrcCPUTemperature = 0x00000080
	SrcCPUUsage       = 0x00000090
	SrcCPUPower       = 0x00000100
)

// Offsets within one MAHM_SHARED_MEMORY_GPU_ENTRY (the array that follows
// immediately after the main entries array, per the v2.0 header's
// dwNumGpuEntries/dwGpuEntrySize).
const (
	gpuEntryOffMemAmount = maxPath * 5 // szGpuId+szFamily+szDevice+szDriver+szBIOS
)

const gpuEntrySizeFallback = gpuEntryOffMemAmount + 4

// notAvailable mirrors FLT_MAX, which Afterburner writes into a data
// source's value when that source currently has no data.
const notAvailable = 3.402823466e+38

// Entry is one decoded MAHM_SHARED_MEMORY_ENTRY.
type Entry struct {
	SrcID uint32
	GPU   uint32 // GPU index, or 0xFFFFFFFF for global (non-GPU-specific) sources
	Units string
	Value float32
	OK    bool // false if the source reported "not available"
}

// Snapshot is a decoded read of the whole entries array at one point in
// time.
type Snapshot struct {
	Available bool
	Entries   []Entry
	// GPUMemAmountKB holds the card's total on-board video memory in KB
	// (MAHM_SHARED_MEMORY_GPU_ENTRY.dwMemAmount), indexed by GPU index.
	GPUMemAmountKB []uint32
}

// Get returns the first entry matching srcID and gpu (gpu is ignored for
// CPU/global sources; pass 0 for the primary GPU).
func (s Snapshot) Get(srcID uint32, gpu uint32) (Entry, bool) {
	for _, e := range s.Entries {
		if e.SrcID != srcID {
			continue
		}
		if e.GPU != 0xFFFFFFFF && e.GPU != gpu {
			continue
		}
		return e, true
	}
	return Entry{}, false
}

// GPUMemTotalMB returns the given GPU's total on-board video memory in MB
// (dwMemAmount is in KB). Returns false if that GPU's entry was not present,
// or if Afterburner has not populated dwMemAmount for it (the SDK header
// notes it "can be empty if data is not available", observed in practice
// for GPUs newer than the installed Afterburner's device database).
func (s Snapshot) GPUMemTotalMB(gpu uint32) (uint32, bool) {
	if int(gpu) >= len(s.GPUMemAmountKB) || s.GPUMemAmountKB[gpu] == 0 {
		return 0, false
	}
	return s.GPUMemAmountKB[gpu] / 1024, true
}

// Reader keeps a lazily-opened, reconnecting handle to the Afterburner
// hardware monitoring shared memory section.
type Reader struct {
	mu    sync.Mutex
	m     *winshm.Mapping
	mutex *winshm.Mutex
}

func NewReader() *Reader { return &Reader{} }

func (r *Reader) Close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.m != nil {
		r.m.Close()
		r.m = nil
	}
	if r.mutex != nil {
		r.mutex.Close()
		r.mutex = nil
	}
}

func (r *Reader) ensure() (*winshm.Mapping, bool) {
	if r.m != nil {
		return r.m, true
	}
	m, err := winshm.OpenMapping(mappingName)
	if err != nil {
		return nil, false
	}
	if m.U32(hdrOffSignature) != signatureMAHM {
		m.Close()
		return nil, false
	}
	// The mutex is best-effort: if it can't be opened we still read the
	// mapping directly, accepting a small chance of a torn read.
	mtx, _ := winshm.OpenMutex(mutexName)

	r.m = m
	r.mutex = mtx
	return r.m, true
}

// Available reports whether Afterburner's shared memory is currently
// reachable (i.e. Afterburner is running with hardware monitoring
// enabled).
func (r *Reader) Available() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	_, ok := r.ensure()
	return ok
}

// Read decodes the current entries array.
func (r *Reader) Read() Snapshot {
	r.mu.Lock()
	defer r.mu.Unlock()

	m, ok := r.ensure()
	if !ok {
		return Snapshot{}
	}
	if m.U32(hdrOffSignature) != signatureMAHM {
		// Producer likely restarted; drop and retry on next call.
		m.Close()
		r.m = nil
		return Snapshot{}
	}

	if r.mutex.Lock() {
		defer r.mutex.Unlock()
	}

	numEntries := m.U32(hdrOffNumEntries)
	entrySize := m.U32(hdrOffEntrySize)
	if entrySize == 0 || entrySize > 65536 {
		entrySize = entrySizeFallback
	}
	if numEntries > 4096 {
		// Sanity cap: something is wrong (stale/corrupt mapping), refuse
		// to iterate an implausible count and force a reconnect next call.
		m.Close()
		r.m = nil
		return Snapshot{}
	}

	entries := make([]Entry, 0, numEntries)
	for i := uint32(0); i < numEntries; i++ {
		base := headerSize + uintptr(i)*uintptr(entrySize)

		val := m.F32(base + entryOffData)
		ok := val < notAvailable*0.999999

		entries = append(entries, Entry{
			SrcID: m.U32(base + entryOffSrcId),
			GPU:   m.U32(base + entryOffGpu),
			Units: strings.TrimSpace(m.CString(base+entryOffSrcUnits, maxPath)),
			Value: val,
			OK:    ok,
		})
	}

	numGpuEntries := m.U32(hdrOffNumGpuEntries)
	gpuEntrySize := m.U32(hdrOffGpuEntrySize)
	if gpuEntrySize == 0 || gpuEntrySize > 65536 {
		gpuEntrySize = gpuEntrySizeFallback
	}
	if numGpuEntries > 64 {
		// Sanity cap: implausible GPU count means a stale/corrupt header;
		// skip decoding rather than reading garbage.
		numGpuEntries = 0
	}

	gpuArrBase := headerSize + uintptr(numEntries)*uintptr(entrySize)
	gpuMem := make([]uint32, numGpuEntries)
	for i := uint32(0); i < numGpuEntries; i++ {
		base := gpuArrBase + uintptr(i)*uintptr(gpuEntrySize)
		gpuMem[i] = m.U32(base + gpuEntryOffMemAmount)
	}

	return Snapshot{Available: true, Entries: entries, GPUMemAmountKB: gpuMem}
}
