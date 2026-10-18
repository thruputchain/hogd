package main

import (
	"fmt"
	"net/http"
	"testing"
	"time"
)

func TestReplayClientsPreserveConnectionBudget(t *testing.T) {
	for _, slots := range []int{1, 15, 16, 17, 26624, 26625} {
		t.Run(fmt.Sprint(slots), func(t *testing.T) {
			config := Config{Outstanding: slots, ReceiptWorkers: 16, Timeout: time.Second}
			clients, control, closeClients := replayClients(config)
			defer closeClients()
			seen := map[http.RoundTripper]bool{control.Transport: true}
			total := 0
			for _, client := range clients {
				if seen[client.Transport] {
					t.Fatal("clients share a connection-pool lock")
				}
				seen[client.Transport] = true
				transport := client.Transport.(*http.Transport)
				if transport.MaxConnsPerHost < 1 || transport.MaxIdleConns != transport.MaxConnsPerHost || transport.MaxIdleConnsPerHost != transport.MaxConnsPerHost || client.Timeout != config.Timeout {
					t.Fatal("invalid bounded connection pool or timeout")
				}
				total += transport.MaxConnsPerHost
			}
			if total != slots {
				t.Fatalf("submission connection budget: got %d, want %d", total, slots)
			}
			if transport := control.Transport.(*http.Transport); transport.MaxConnsPerHost != config.ReceiptWorkers+32 {
				t.Fatal("receipt/telemetry connection reserve changed")
			}
		})
	}
}
