# rtss_api

A small Windows-only HTTP server that exposes live GPU/CPU/RAM/FPS telemetry
by reading the shared memory that **RivaTuner Statistics Server (RTSS)** and
**MSI Afterburner** already maintain on this machine. It's meant to be
polled frequently by a small WiFi client (e.g. an ESP32-S3 desk display)
that draws a live FPS graph and shows current GPU/CPU stats.

No admin rights, no game injection, no extra drivers: this only opens the
named shared memory sections these two programs already publish for their
own on-screen display, and republishes the numbers over plain HTTP as JSON
or as a compact binary blob.

## How it actually gets the data

This is important to understand because it explains what you need running,
and why some fields can be `null`:

| Data | Source | Notes |
|---|---|---|
| FPS, frametime | **RTSS** shared memory (`RTSSSharedMemoryV2`) | Requires RTSS running **and** a hooked 3D app (D3D9-12/OpenGL/Vulkan) in the foreground. |
| GPU temp/load/clock/power, GPU memory used | **MSI Afterburner** shared memory (`MAHMSharedMemory`) | RTSS itself does **not** monitor hardware sensors — Afterburner does. Afterburner must be running, **and** each sensor must be individually checked/enabled on Afterburner's *Settings → Monitoring* tab, or it simply won't appear in shared memory. GPU memory used comes from Afterburner's "Memory usage" sensor (`MONITORING_SOURCE_ID_MEMORY_USAGE`) — despite the generic name, this is video memory, not system RAM. |
| CPU temp/load/power | **MSI Afterburner** shared memory (`MAHMSharedMemory`) | Same as above: must be checked on the *Monitoring* tab. |
| GPU memory total | **MSI Afterburner** shared memory, GPU entry header (`dwMemAmount`) | The card's on-board VRAM size, reported directly by Afterburner's GPU detection — not a checkbox sensor, always present whenever Afterburner is running. |
| RAM used (MB), RAM total (MB) | Windows `GlobalMemoryStatusEx` | Queried directly, doesn't depend on RTSS/Afterburner at all. |
| RAM clock speed (MHz) | WMI (`Win32_PhysicalMemory.Speed`) | Queried once at startup and cached — DRAM speed doesn't change at runtime. Neither RTSS nor Afterburner expose this (Afterburner's "Memory clock" is GPU VRAM clock, not system RAM). |

So: **RTSS alone is not enough** for GPU/CPU sensors — you need Afterburner
running too (both are already installed on this machine). If you only ever
care about FPS, RTSS alone is fine.

The struct layouts used to read both shared memory sections were
transcribed directly from the official SDK headers shipped with RTSS/
Afterburner (`...\RivaTuner Statistics Server\SDK\Include\RTSSSharedMemory.h`
and `...\MSI Afterburner\SDK\Include\MSIAB\MAHMSharedMemory.h`), not
reverse engineered blind, so the offsets match the exact versions installed
on this machine.

## Requirements

- Windows (uses `kernel32.dll` file-mapping APIs directly; no non-stdlib
  Go dependencies).
- RTSS running (for FPS) and/or MSI Afterburner running (for hardware
  sensors). Both are typically set to start with Windows.
- In Afterburner: **Settings → Monitoring**, tick the checkbox next to
  every sensor you want exposed (GPU temperature, GPU usage, Core clock,
  GPU power, CPU temperature, CPU usage, CPU power, **Memory usage** — the
  last one is GPU video memory used, despite the generic name). Unchecked
  sensors are invisible to this API, not just hidden in Afterburner's own
  UI.
- In RTSS: On-Screen Display enabled (default) so it hooks running 3D
  applications. No specific OSD layout is required — this API reads raw
  shared memory, not the rendered OSD text.

## Build & run

```powershell
cd rtss_api
go build -o rtss_api.exe .
.\rtss_api.exe -addr :8099 -gpu 0
```

Flags:
- `-addr` — HTTP listen address (default `:8099`)
- `-gpu` — default Afterburner GPU index to report when a request doesn't
  specify `?gpu=` (default `0`, i.e. the primary GPU)

Run it once with Windows startup (Task Scheduler, "run at logon") so it's
always up when your desk display polls it — it's cheap to leave running,
each request only re-reads already-mapped memory (microseconds), no
extra polling of RTSS/Afterburner is done in the background.

## API

Base URL: `http://<pc-ip>:8099`

### `GET /api/v1/health`

Quick check of which sources are currently reachable — use this to decide
whether to show "N/A" instead of retrying constantly.

```json
{ "status": "ok", "rtss": true, "afterburner": true }
```

### `GET /api/v1/metrics`

Full snapshot as JSON. Every numeric field is `null` when that particular
value is currently unavailable (source not running, sensor not checked in
Afterburner, or no FPS data because no 3D app is in the foreground).

```json
{
  "ts": 1789117200307,
  "sources": { "rtss": true, "afterburner": true },
  "gpu": {
    "temperature_c": 65.0,
    "load_percent": 87.0,
    "core_clock_mhz": 1905.0,
    "power_watts": 210.6,
    "mem_clock_mhz": 1750.0,
    "mem_total_mb": 12288,
    "mem_used_mb": 4096
  },
  "cpu": {
    "temperature_c": 61.0,
    "load_percent": 34.2,
    "power_watts": 45.8
  },
  "mem": {
    "used_mb": 16234,
    "total_mb": 32625,
    "freq_mhz": 3200
  },
  "fps": {
    "process": "cyberpunk2077.exe",
    "pid": 12345,
    "current": 91.2,
    "avg": 88.5,
    "min": 45.0,
    "max": 120.0
  }
}
```

Query parameters (all optional):
- `process=<exe name>` — pick FPS stats for a specific process, e.g.
  `?process=cyberpunk2077.exe` (case-insensitive, exact executable name
  match). Overrides the default foreground-app selection.
- `pid=<number>` — pick FPS stats by process ID instead of name.
- `gpu=<index>` — which GPU to report hardware stats for (0-based;
  overrides `-gpu`).

If `process`/`pid` are omitted, the API reports whichever application RTSS
currently considers the foreground 3D app — this is normally what you
want for a "what am I playing right now" display.

### `GET /api/v1/metrics.bin` — binary polling endpoint

**This is the one to use from the ESP32.** It returns a fixed 72-byte
little-endian binary blob instead of JSON — no string parsing needed on
the microcontroller, smaller over WiFi, and safe to poll at high
frequency (each request costs the server only a few memory reads; there's
no meaningful upper bound on request rate from a single low-power client).

Accepts the same `?process=`, `?pid=`, `?gpu=` query parameters as
`/metrics`.

Layout (all little-endian, no padding — this exact order):

| Offset | Bytes | Type | Field |
|---|---|---|---|
| 0 | 4 | uint32 | magic = `0x42415452` ("RTAB" bytes) |
| 4 | 4 | uint32 | seq — increments on every request served; use to detect stale/duplicate reads |
| 8 | 4 | float32 | gpu_temp_c |
| 12 | 4 | float32 | gpu_load_pct |
| 16 | 4 | float32 | gpu_clock_mhz |
| 20 | 4 | float32 | gpu_power_w |
| 24 | 4 | float32 | cpu_temp_c |
| 28 | 4 | float32 | cpu_load_pct |
| 32 | 4 | float32 | cpu_power_w |
| 36 | 4 | uint32 | mem_used_mb |
| 40 | 4 | uint32 | mem_total_mb |
| 44 | 4 | uint32 | mem_freq_mhz |
| 48 | 4 | float32 | fps_current |
| 52 | 4 | float32 | fps_avg |
| 56 | 4 | uint32 | pid — process ID the FPS fields refer to (0 if none) |
| 60 | 4 | float32 | gpu_mem_clock_mhz — video memory clock |
| 64 | 4 | uint32 | gpu_mem_total_mb — the card's video memory |
| 68 | 4 | uint32 | gpu_mem_used_mb — video memory currently in use |

**Unavailable values are encoded as `-1.0`** for float fields (every metric
here is physically non-negative, so this is unambiguous) and `0` for the
integer fields. Always check for `-1.0f` / `0` before trusting a value.

Matching C struct for Arduino/ESP-IDF (pack it explicitly since these are
all naturally 4-byte-aligned fields, but be defensive across toolchains):

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t seq;
    float    gpu_temp_c;
    float    gpu_load_pct;
    float    gpu_clock_mhz;
    float    gpu_power_w;
    float    cpu_temp_c;
    float    cpu_load_pct;
    float    cpu_power_w;
    uint32_t mem_used_mb;
    uint32_t mem_total_mb;
    uint32_t mem_freq_mhz;
    float    fps_current;
    float    fps_avg;
    uint32_t pid;
    float    gpu_mem_clock_mhz;
    uint32_t gpu_mem_total_mb;
    uint32_t gpu_mem_used_mb;
} rtss_metrics_t; // 72 bytes
#pragma pack(pop)

#define RTSS_METRICS_MAGIC 0x42415452u
```

Example fetch with `HTTPClient` (Arduino core for ESP32):

```cpp
#include <HTTPClient.h>

bool fetchMetrics(rtss_metrics_t &out) {
  HTTPClient http;
  http.begin("http://192.168.1.50:8099/api/v1/metrics.bin");
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  int n = stream->readBytes((uint8_t *)&out, sizeof(out));
  http.end();

  return n == sizeof(out) && out.magic == RTSS_METRICS_MAGIC;
}
```

## Suggested ESP32-S3 polling design

- Poll `/api/v1/metrics.bin` on a timer, e.g. every **150–250 ms** (4–6 Hz)
  for a smooth-looking live FPS number and graph; drop to 1 Hz for the
  GPU/CPU/RAM tiles if you want to save WiFi airtime and battery — they
  don't need to update as fast as FPS.
- Build the live graph **client-side**: keep a small ring buffer of the
  last N `fps_current` samples on the ESP32 and redraw the sparkline from
  it. The server does not keep FPS history for you (RTSS's own internal
  buffer isn't exposed through this API to keep the response tiny and
  fixed-size).
- Reuse the TCP connection (`HTTPClient` with `http.begin()`/`http.end()`
  per call is fine at these rates; if you want to go faster, keep a
  persistent `WiFiClient` and pipeline plain HTTP/1.1 requests instead of
  reconnecting each time).
- Use `seq` to detect and skip a duplicate/stale frame if you ever add
  retry logic.
- Treat `sources` (on `/metrics`) or a `-1.0`/`0` sentinel (on
  `/metrics.bin`) as "show placeholder", not as an error worth logging
  loudly — it's completely normal for `fps.*` to be unavailable whenever
  you're on the desktop with no game focused.

## Multiple GPUs

Afterburner enumerates GPUs by index (0 = primary, by default the one
Windows considers GPU0). Pass `?gpu=1` etc. to read a secondary GPU. The
`-gpu` flag sets the server-wide default.

## Troubleshooting

- **`sources.afterburner` is always `false`**: Afterburner isn't running,
  or "Enable hardware monitoring / shared memory" got turned off somewhere
  in its settings. Start Afterburner (it can run minimized to tray).
- **GPU/CPU fields are `null` even though Afterburner is running**: the
  corresponding sensor isn't checked on Afterburner's *Monitoring* tab —
  tick it there. `cpu_power_w`/`cpu_temp_c` in particular are only
  available if your CPU/motherboard combo is supported by Afterburner's
  sensor backend; some CPUs simply don't expose power reporting to it.
- **`sources.rtss` is `false`**: either RTSS isn't running, or — more
  commonly — there is currently no foreground 3D application for RTSS to
  report on (e.g. you're on the desktop). This is expected, not a bug.
- **FPS numbers for the wrong game**: pass `?process=<exe>` explicitly
  instead of relying on RTSS's foreground detection.

## Project layout

```
main.go                     - HTTP server entry point, flags
internal/winshm/            - raw Win32 file-mapping + mutex helpers (stdlib only)
internal/rtss/               - RTSS shared memory reader (FPS/frametime/lows)
internal/mahm/                - Afterburner hardware monitoring shared memory reader
internal/sysmem/              - system RAM usage (WinAPI) + RAM speed (WMI, cached)
internal/api/                 - aggregation + HTTP handlers (JSON and binary)
```

## Why Go

Everything here is small syscalls into already-mapped memory (no polling
loop, no background thread hitting RTSS/Afterburner on its own — every
HTTP request just re-reads the live shared memory directly), so pretty much
any language would be "fast enough" at the couple-Hz rates an ESP32 desk
display needs. Go was picked because:

- Single static `.exe`, no runtime to install, trivial to run as a
  scheduled task on the gaming PC.
- Direct `kernel32.dll` syscalls via the standard library only — no
  external dependencies (`go build` is all you need, no `go get`, no
  vendoring, no version drift).
- `net/http` comfortably handles far more concurrent requests than an
  ESP32 will ever generate, with predictable, low, GC-friendly latency at
  this data size.
- Cross-compiling isn't actually relevant here (the server can only ever
  run on the Windows gaming PC — the whole point is reading local shared
  memory), so Go's usual "build once, run anywhere" pitch isn't the
  deciding factor; its ergonomics for "small static binary that does raw
  Win32 syscalls and a JSON API" are.

If you'd rather not use Go, the realistic alternatives and their
trade-offs:
- **C#/.NET** — arguably the most "native" choice on Windows and there are
  existing NuGet packages for RTSS/MAHM shared memory, but ships either a
  much larger self-contained binary or a runtime dependency, and pulling
  in a third-party shared-memory package means trusting its offset
  correctness instead of reading the vendor header yourself.
- **Rust** — same raw-syscall approach as this Go version, single static
  binary, marginally lower latency (irrelevant at these rates); more
  boilerplate for very little practical benefit here.
- **Python** — fastest to prototype, but `ctypes`-based Win32 struct
  reads are slower to write correctly and it needs a bundled interpreter
  or PyInstaller to run standalone; fine if you want to iterate on the
  offsets interactively before "productionizing" in Go.

Go was the best fit for "small, dependency-free, always-on background
service" here, so that's what got built.
