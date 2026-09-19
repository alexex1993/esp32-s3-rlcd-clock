//go:build windows

// Package winshm provides minimal, dependency-free wrappers around the
// Win32 APIs needed to open a named, read-only file mapping (shared
// memory section) and to synchronize reads against it with a named mutex.
// It intentionally avoids golang.org/x/sys/windows so this project builds
// with the standard library only.
package winshm

import (
	"fmt"
	"syscall"
	"unsafe"
)

var (
	modkernel32 = syscall.NewLazyDLL("kernel32.dll")

	procOpenFileMappingW    = modkernel32.NewProc("OpenFileMappingW")
	procMapViewOfFile       = modkernel32.NewProc("MapViewOfFile")
	procUnmapViewOfFile     = modkernel32.NewProc("UnmapViewOfFile")
	procCloseHandle         = modkernel32.NewProc("CloseHandle")
	procOpenMutexW          = modkernel32.NewProc("OpenMutexW")
	procWaitForSingleObject = modkernel32.NewProc("WaitForSingleObject")
	procReleaseMutex        = modkernel32.NewProc("ReleaseMutex")
)

const (
	fileMapRead = 0x0004
	synchronize = 0x00100000

	// waitTimeoutMS bounds how long a request will block trying to acquire
	// a producer's mutex. We prefer a possibly-torn read over stalling an
	// HTTP handler if the producer process died while holding the lock.
	waitTimeoutMS = 30
	waitObject0   = 0
)

// Mapping is an open, memory-mapped, read-only named section.
type Mapping struct {
	handle uintptr
	addr   unsafe.Pointer
}

// OpenMapping opens an existing named file mapping (created by another
// process, e.g. RTSS.exe or MSIAfterburner.exe) and maps it fully into this
// process' address space for reading.
func OpenMapping(name string) (*Mapping, error) {
	namep, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}

	h, _, _ := procOpenFileMappingW.Call(uintptr(fileMapRead), 0, uintptr(unsafe.Pointer(namep)))
	if h == 0 {
		return nil, fmt.Errorf("winshm: OpenFileMapping(%q) failed (producer not running?)", name)
	}

	addr, _, _ := procMapViewOfFile.Call(h, uintptr(fileMapRead), 0, 0, 0)
	if addr == 0 {
		procCloseHandle.Call(h)
		return nil, fmt.Errorf("winshm: MapViewOfFile(%q) failed", name)
	}

	// addr is a fresh base address for this process' address space, just
	// returned by MapViewOfFile; converting it to unsafe.Pointer here is
	// safe (this triggers a known `go vet` false positive because
	// syscall.LazyProc.Call, unlike syscall.Syscall, isn't special-cased
	// by the unsafeptr checker).
	return &Mapping{handle: h, addr: unsafe.Pointer(addr)}, nil
}

// Close unmaps the view and closes the section handle.
func (m *Mapping) Close() {
	if m == nil {
		return
	}
	if m.addr != nil {
		procUnmapViewOfFile.Call(uintptr(m.addr))
	}
	if m.handle != 0 {
		procCloseHandle.Call(m.handle)
	}
}

// U32 reads a little-endian uint32 at byte offset off from the mapping base.
func (m *Mapping) U32(off uintptr) uint32 {
	return *(*uint32)(unsafe.Pointer(uintptr(m.addr) + off))
}

// I32 reads a little-endian int32 at byte offset off from the mapping base.
func (m *Mapping) I32(off uintptr) int32 {
	return *(*int32)(unsafe.Pointer(uintptr(m.addr) + off))
}

// F32 reads a float32 at byte offset off from the mapping base.
func (m *Mapping) F32(off uintptr) float32 {
	return *(*float32)(unsafe.Pointer(uintptr(m.addr) + off))
}

// CString reads a NUL-terminated string of at most maxLen bytes starting at
// byte offset off from the mapping base.
func (m *Mapping) CString(off uintptr, maxLen int) string {
	b := unsafe.Slice((*byte)(unsafe.Pointer(uintptr(m.addr)+off)), maxLen)
	n := 0
	for n < maxLen && b[n] != 0 {
		n++
	}
	return string(b[:n])
}

// Mutex wraps a named OS mutex used by some producers (e.g. Afterburner's
// hardware monitoring shared memory) to guard against torn reads while the
// producer is updating the section.
type Mutex struct {
	handle uintptr
}

// OpenMutex opens an existing named mutex. It returns an error if the mutex
// does not exist (the producer process is not running).
func OpenMutex(name string) (*Mutex, error) {
	namep, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	h, _, _ := procOpenMutexW.Call(uintptr(synchronize), 0, uintptr(unsafe.Pointer(namep)))
	if h == 0 {
		return nil, fmt.Errorf("winshm: OpenMutex(%q) failed", name)
	}
	return &Mutex{handle: h}, nil
}

// Lock waits briefly to acquire the mutex. It returns false on timeout
// rather than blocking indefinitely.
func (m *Mutex) Lock() bool {
	if m == nil {
		return false
	}
	r, _, _ := procWaitForSingleObject.Call(m.handle, waitTimeoutMS)
	return r == waitObject0
}

// Unlock releases the mutex. Safe to call even if Lock returned false.
func (m *Mutex) Unlock() {
	if m == nil {
		return
	}
	procReleaseMutex.Call(m.handle)
}

// Close closes the mutex handle.
func (m *Mutex) Close() {
	if m == nil || m.handle == 0 {
		return
	}
	procCloseHandle.Call(m.handle)
}
