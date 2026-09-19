//go:build windows

// Command rtss_api exposes RTSS/MSI Afterburner telemetry (GPU/CPU/RAM/FPS,
// including 1% and 0.1% FPS lows) over a small HTTP API, designed to be
// polled frequently by resource-constrained WiFi clients such as an
// ESP32-S3 desk display. See README.md for the full API reference.
package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"time"

	"rtss_api/internal/api"
)

func main() {
	addr := flag.String("addr", ":8099", "HTTP listen address")
	gpu := flag.Uint("gpu", 0, "default Afterburner GPU index to report")
	flag.Parse()

	srv := api.NewServer(uint32(*gpu))
	defer srv.Close()

	mux := http.NewServeMux()
	srv.Routes(mux)

	httpServer := &http.Server{
		Addr:              *addr,
		Handler:           mux,
		ReadHeaderTimeout: 3 * time.Second,
		ReadTimeout:       5 * time.Second,
		WriteTimeout:      5 * time.Second,
		IdleTimeout:       30 * time.Second,
	}

	go func() {
		log.Printf("rtss_api listening on %s", *addr)
		log.Printf("  GET /api/v1/metrics      - full JSON snapshot")
		log.Printf("  GET /api/v1/metrics.bin  - compact 76-byte binary snapshot")
		log.Printf("  GET /api/v1/health       - source availability check")
		if err := httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("http server error: %v", err)
		}
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt)
	<-stop

	log.Println("shutting down...")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(ctx)
}
