//go:build windows

// Package sysmem reports system RAM usage and clock speed.
//
// Neither RTSS nor Afterburner track system RAM used to the byte or its
// clock speed (Afterburner's "Memory usage"/"Memory clock" sources are
// GPU VRAM, not system RAM), so this data comes directly from Windows
// instead: GlobalMemoryStatusEx for live usage, and a one-time WMI query
// for the DRAM speed (it does not change at runtime).
package sysmem

import (
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

var (
	modkernel32              = syscall.NewLazyDLL("kernel32.dll")
	procGlobalMemoryStatusEx = modkernel32.NewProc("GlobalMemoryStatusEx")
)

// memoryStatusEx mirrors the Win32 MEMORYSTATUSEX struct field-for-field.
type memoryStatusEx struct {
	Length               uint32
	MemoryLoad           uint32
	TotalPhys            uint64
	AvailPhys            uint64
	TotalPageFile        uint64
	AvailPageFile        uint64
	TotalVirtual         uint64
	AvailVirtual         uint64
	AvailExtendedVirtual uint64
}

// Usage is the current system RAM usage.
type Usage struct {
	UsedMB  uint64
	TotalMB uint64
}

// ReadUsage queries current physical memory usage.
func ReadUsage() (Usage, bool) {
	var m memoryStatusEx
	m.Length = uint32(unsafe.Sizeof(m))
	r, _, _ := procGlobalMemoryStatusEx.Call(uintptr(unsafe.Pointer(&m)))
	if r == 0 {
		return Usage{}, false
	}
	const mb = 1024 * 1024
	total := m.TotalPhys / mb
	avail := m.AvailPhys / mb
	used := uint64(0)
	if total > avail {
		used = total - avail
	}
	return Usage{UsedMB: used, TotalMB: total}, true
}

var (
	speedOnce sync.Once
	speedMHz  uint32
)

// RAMSpeedMHz returns the DRAM clock speed reported by the first
// installed memory module, queried once via WMI and cached for the life
// of the process. Returns 0 if it could not be determined.
func RAMSpeedMHz() uint32 {
	speedOnce.Do(func() {
		speedMHz = queryRAMSpeedMHz()
	})
	return speedMHz
}

func queryRAMSpeedMHz() uint32 {
	cmd := exec.Command("powershell", "-NoProfile", "-NonInteractive", "-Command",
		"(Get-CimInstance -ClassName Win32_PhysicalMemory | Select-Object -First 1 -ExpandProperty Speed)")
	out, err := cmd.Output()
	if err != nil {
		return 0
	}
	s := strings.TrimSpace(string(out))
	v, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return 0
	}
	return uint32(v)
}
