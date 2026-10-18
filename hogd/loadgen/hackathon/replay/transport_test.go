package main

import (
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestHTTP2MultiplexesWithoutChangingControlProtocol(t *testing.T) {
	const parallel = 8
	var connections atomic.Int64
	arrived, release := make(chan struct{}, parallel), make(chan struct{})
	var once sync.Once
	unblock := func() { once.Do(func() { close(release) }) }
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.ProtoMajor != 2 {
			t.Errorf("submission used %s", r.Proto)
		}
		if r.URL.Path != "/warm" {
			arrived <- struct{}{}
			select {
			case <-release:
			case <-r.Context().Done():
			}
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	server.Config.Protocols = new(http.Protocols)
	server.Config.Protocols.SetUnencryptedHTTP2(true)
	server.Config.ConnState = func(_ net.Conn, state http.ConnState) {
		if state == http.StateNew {
			connections.Add(1)
		}
	}
	server.Start()
	defer server.Close()
	defer unblock()
	clients, control, closeClients := replayClients(Config{Outstanding: 16, ReceiptWorkers: 1, Timeout: 5 * time.Second, SubmissionHTTP2: true})
	defer closeClients()
	get := func(client *http.Client, url string) error {
		response, err := client.Get(url)
		if err == nil {
			response.Body.Close()
		}
		return err
	}
	// Establish a connection before overlapping requests, avoiding cold dial races.
	if err := get(clients[0], server.URL+"/warm"); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, parallel)
	for i := 0; i < parallel; i++ {
		go func() { done <- get(clients[0], server.URL) }()
	}
	timer := time.NewTimer(5 * time.Second)
	defer timer.Stop()
	for i := 0; i < parallel; i++ {
		select {
		case <-arrived:
		case <-timer.C:
			t.Fatal("HTTP/2 requests did not overlap")
		}
	}
	if connections.Load() != 1 {
		t.Fatalf("opened %d connections for %d overlapping requests", connections.Load(), parallel)
	}
	metrics := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.ProtoMajor != 1 {
			t.Errorf("control pool used %s", r.Proto)
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	defer metrics.Close()
	if err := get(control, metrics.URL); err != nil {
		t.Fatalf("HTTP/1 telemetry failed while submissions were in flight: %v", err)
	}
	unblock()
	for i := 0; i < parallel; i++ {
		if err := <-done; err != nil {
			t.Fatal(err)
		}
	}
}

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
