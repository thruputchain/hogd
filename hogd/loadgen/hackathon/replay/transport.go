package main

import "net/http"

// replayClients divides the submission connection budget across independent
// transports. A single Transport serializes all idle-connection handoffs under
// one mutex, which otherwise limits the offered load at large client windows.
// Receipt polling and telemetry have their own reserved connection budget.
// Run's global permits still bound submissions across all transports.
func replayClients(c Config) ([]*http.Client, *http.Client, func()) {
	var transports []*http.Transport
	newClient := func(connections int, http2 bool) *http.Client {
		transport := &http.Transport{
			MaxIdleConns:        connections,
			MaxIdleConnsPerHost: connections,
			MaxConnsPerHost:     connections,
		}
		if http2 {
			transport.Protocols = new(http.Protocols)
			transport.Protocols.SetHTTP2(true)
			transport.Protocols.SetUnencryptedHTTP2(true)
		}
		transports = append(transports, transport)
		return &http.Client{Transport: transport, Timeout: c.Timeout}
	}
	clients := make([]*http.Client, min(16, c.Outstanding))
	for i := range clients {
		connections := c.Outstanding / len(clients)
		if i < c.Outstanding%len(clients) {
			connections++
		}
		clients[i] = newClient(connections, c.SubmissionHTTP2)
	}
	// Setup, receipts and HTTP/1-only telemetry endpoints use the control pool.
	control := newClient(c.ReceiptWorkers+32, false)
	return clients, control, func() {
		for _, transport := range transports {
			transport.CloseIdleConnections()
		}
	}
}
