// Replay an offline signed corpus through a live RPC gateway.
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/thruputchain/lib-go/libxdr"
	proto "github.com/thruputchain/thruput-protocol-go"
)

type Config struct {
	Corpus, Output, URL         string
	Outstanding                 int
	ReceiptWorkers              int
	Drain, Timeout, MaxLateness time.Duration
	Metrics                     map[string]string
}
type Manifest struct {
	SchemaVersion        int      `json:"schemaVersion"`
	Operations           int      `json:"operations"`
	DurationSeconds      int      `json:"durationSeconds"`
	InitializationHashes []string `json:"initializationHashes"`
}
type Operation struct {
	DueNs      int64  `json:"dueNs"`
	Phase      int    `json:"phase"`
	Kind       string `json:"kind"`
	App        int    `json:"app"`
	User       int    `json:"user"`
	Dependency int    `json:"dependency"`
	Hash       string `json:"hash"`
	Raw        string `json:"raw"`
}
type Result struct {
	Operation
	RawOmitted bool   `json:"rawOmitted"`
	StartNs    int64  `json:"startNs"`
	EndNs      int64  `json:"endNs"`
	Outcome    string `json:"outcome"`
	Error      string `json:"error,omitempty"`
	HTTPStatus int    `json:"httpStatus,omitempty"`
	ReceiptNs  int64  `json:"receiptNs,omitempty"`
	Height     string `json:"height,omitempty"`
	Execution  string `json:"execution,omitempty"`
	GasUsed    string `json:"gasUsed,omitempty"`
}
type rpcRequest struct {
	JSONRPC string   `json:"jsonrpc"`
	ID      int      `json:"id"`
	Method  string   `json:"method"`
	Params  []string `json:"params"`
}
type rpcResponse struct {
	ID     int             `json:"id"`
	Result json.RawMessage `json:"result"`
	Error  json.RawMessage `json:"error"`
}
type receipt struct {
	Hash   string `json:"txHash"`
	Height string `json:"height"`
	Status struct {
		Result string `json:"result"`
	} `json:"status"`
	GasUsed string `json:"gasUsed"`
}

type httpStatusError struct{ code int }

func (e httpStatusError) Error() string { return fmt.Sprintf("HTTP %d", e.code) }

func needsReceipt(r Result) bool {
	// HTTP 429 is a rejection before gateway admission. Transport failures and
	// timeouts remain ambiguous and must still be reconciled by transaction ID.
	return r.StartNs > 0 && r.ReceiptNs == 0 && r.HTTPStatus != http.StatusTooManyRequests
}

func request(id int, method, value string) rpcRequest {
	return rpcRequest{"2.0", id, "groundhog_" + method, []string{value}}
}
func rpc(ctx context.Context, client *http.Client, url string, body any, result any) error {
	data, err := json.Marshal(body)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(data))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	response, err := client.Do(req)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != 200 {
		return httpStatusError{code: response.StatusCode}
	}
	return json.NewDecoder(io.LimitReader(response.Body, 32<<20)).Decode(result)
}
func load(c Config) (Manifest, []Operation, error) {
	var m Manifest
	data, err := os.ReadFile(filepath.Join(c.Corpus, "manifest.json"))
	if err != nil {
		return m, nil, err
	}
	if err = json.Unmarshal(data, &m); err != nil {
		return m, nil, err
	}
	if m.SchemaVersion != 1 || m.Operations <= 0 || m.DurationSeconds <= 0 {
		return m, nil, fmt.Errorf("invalid manifest")
	}
	file, err := os.Open(filepath.Join(c.Corpus, "transactions.jsonl"))
	if err != nil {
		return m, nil, err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 65536), 32<<20)
	ops := make([]Operation, 0, m.Operations)
	hashes := map[string]bool{}
	for scanner.Scan() {
		var op Operation
		if err = json.Unmarshal(scanner.Bytes(), &op); err != nil {
			return m, nil, err
		}
		if op.DueNs < 0 || op.DueNs >= int64(m.DurationSeconds)*int64(time.Second) || (len(ops) > 0 && op.DueNs < ops[len(ops)-1].DueNs) || op.Phase < 0 || op.Phase > 4 || op.App < 0 || op.User < 0 || (op.Kind != "call" && op.Kind != "auth" && op.Kind != "deploy") {
			return m, nil, fmt.Errorf("invalid operation %d", len(ops))
		}
		if !strings.HasPrefix(op.Raw, "0x") {
			return m, nil, fmt.Errorf("invalid raw transaction")
		}
		raw, err := hex.DecodeString(op.Raw[2:])
		if err != nil {
			return m, nil, err
		}
		var tx proto.SignedTransaction
		if err = libxdr.UnmarshalXdrEOF(proto.XDR_SignedTransaction(&tx), raw); err != nil {
			return m, nil, err
		}
		hash := hashTransaction(&tx)
		if op.Hash != "0x"+hex.EncodeToString(hash[:]) || hashes[op.Hash] {
			return m, nil, fmt.Errorf("invalid or duplicate hash")
		}
		hashes[op.Hash] = true
		ops = append(ops, op)
	}
	if err = scanner.Err(); err != nil {
		return m, nil, err
	}
	if len(ops) != m.Operations {
		return m, nil, fmt.Errorf("operation count mismatch")
	}
	return m, ops, nil
}

// Run bounds concurrency without pacing arrivals by completions. A transport
// timeout is ambiguous and remains eligible for receipt reconciliation.
func Run(ctx context.Context, c Config) error {
	if c.Outstanding < 1 || c.ReceiptWorkers < 1 || c.Drain <= 0 || c.Timeout <= 0 || c.MaxLateness <= 0 || c.URL == "" {
		return fmt.Errorf("invalid replay configuration")
	}
	m, ops, err := load(c)
	if err != nil {
		return err
	}
	if err = os.Mkdir(c.Output, 0755); err != nil {
		return err
	}
	configFile, _ := json.MarshalIndent(c, "", "  ")
	if err = os.WriteFile(filepath.Join(c.Output, "config.json"), configFile, 0644); err != nil {
		return err
	}
	// Receipt workers and telemetry must retain connections even when every
	// submission slot is occupied, including runs configured with many pollers.
	connections := c.Outstanding + c.ReceiptWorkers + 32
	transport := &http.Transport{MaxIdleConns: connections, MaxIdleConnsPerHost: connections, MaxConnsPerHost: connections}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: c.Timeout}
	// Setup uses the same gateway as measured submissions and completes before
	// the arrival clock starts.
	if err = initializeApps(ctx, client, c, m); err != nil {
		return err
	}
	results := make([]Result, len(ops))
	for i, op := range ops {
		results[i] = Result{Operation: op, RawOmitted: true, Outcome: "unsent"}
		results[i].Raw = ""
	}
	var mu sync.Mutex
	deployed := map[int]bool{}
	var usageStart syscall.Rusage
	syscall.Getrusage(syscall.RUSAGE_SELF, &usageStart)
	start := time.Now()
	metadata, _ := json.Marshal(map[string]any{"schemaVersion": 1, "startedUnixNs": start.UnixNano(), "ingress": "rpc-gateway", "durationSeconds": m.DurationSeconds})
	if err = os.WriteFile(filepath.Join(c.Output, "run.json"), metadata, 0644); err != nil {
		return err
	}
	runCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	var background sync.WaitGroup
	background.Add(1)
	go func() {
		defer background.Done()
		pollReceipts(runCtx, client, c.URL, start, results, &mu, deployed, c.ReceiptWorkers)
	}()
	metricErrors := make(chan error, 1)
	background.Add(1)
	go func() { defer background.Done(); metricErrors <- scrape(runCtx, client, c.Output, c.Metrics) }()
	permits := make(chan struct{}, c.Outstanding)
	var submits sync.WaitGroup
	for i, op := range ops {
		if ctx.Err() != nil {
			break
		}
		due := start.Add(time.Duration(op.DueNs))
		if delay := time.Until(due); delay > 0 {
			timer := time.NewTimer(delay)
			select {
			case <-ctx.Done():
				timer.Stop()
			case <-timer.C:
			}
		}
		mu.Lock()
		if ctx.Err() != nil {
			mu.Unlock()
			break
		}
		if time.Since(due) > c.MaxLateness {
			results[i].Outcome = "missed_arrival"
			mu.Unlock()
			continue
		}
		if op.Dependency >= 0 && !deployed[op.Dependency] {
			results[i].Outcome = "dependency_blocked"
			mu.Unlock()
			continue
		}
		select {
		case permits <- struct{}{}:
		default:
			results[i].Outcome = "outstanding_limit"
			mu.Unlock()
			continue
		}
		results[i].StartNs = time.Since(start).Nanoseconds()
		results[i].Outcome = "inflight"
		mu.Unlock()
		submits.Add(1)
		go func(i int, op Operation) {
			defer submits.Done()
			defer func() { <-permits }()
			requestCtx, done := context.WithTimeout(ctx, c.Timeout)
			defer done()
			outcome, e := submit(requestCtx, client, c.URL, op, i)
			mu.Lock()
			results[i].EndNs = time.Since(start).Nanoseconds()
			results[i].Outcome = outcome
			if e != nil {
				results[i].Error = e.Error()
				var status httpStatusError
				if errors.As(e, &status) {
					results[i].HTTPStatus = status.code
				}
			}
			mu.Unlock()
		}(i, op)
	}
	// Preserve the full offered window even if its final operation was blocked.
	timer := time.NewTimer(max(time.Until(start.Add(time.Duration(m.DurationSeconds)*time.Second)), 0))
	select {
	case <-ctx.Done():
		timer.Stop()
	case <-timer.C:
	}
	submits.Wait()
	deadline := time.Now().Add(c.Drain)
	for ctx.Err() == nil && time.Now().Before(deadline) {
		mu.Lock()
		pending := false
		for _, r := range results {
			if needsReceipt(r) {
				pending = true
				break
			}
		}
		mu.Unlock()
		if !pending {
			break
		}
		select {
		case <-ctx.Done():
		case <-time.After(250 * time.Millisecond):
		}
	}
	cancel()
	background.Wait()
	var usageEnd syscall.Rusage
	syscall.Getrusage(syscall.RUSAGE_SELF, &usageEnd)
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	cpuSeconds := float64(usageEnd.Utime.Sec-usageStart.Utime.Sec+usageEnd.Stime.Sec-usageStart.Stime.Sec) + float64(usageEnd.Utime.Usec-usageStart.Utime.Usec+usageEnd.Stime.Usec-usageStart.Stime.Usec)/1e6
	resource, _ := json.Marshal(map[string]any{"cpuSeconds": cpuSeconds, "elapsedSeconds": time.Since(start).Seconds(), "heapBytes": memory.HeapAlloc, "maxRssKiB": usageEnd.Maxrss, "numCPU": runtime.NumCPU(), "gomaxprocs": runtime.GOMAXPROCS(0)})
	if err = os.WriteFile(filepath.Join(c.Output, "generator.json"), resource, 0644); err != nil {
		return err
	}
	file, err := os.Create(filepath.Join(c.Output, "results.jsonl"))
	if err != nil {
		return err
	}
	writer := bufio.NewWriter(file)
	encoder := json.NewEncoder(writer)
	for _, r := range results {
		if err = encoder.Encode(r); err != nil {
			file.Close()
			return err
		}
	}
	if err = writer.Flush(); err != nil {
		file.Close()
		return err
	}
	if err = file.Close(); err != nil {
		return err
	}
	if err = <-metricErrors; err != nil {
		return err
	}
	if ctx.Err() != nil {
		return ctx.Err()
	}
	// Overload remains data; malformed fixtures and committed execution faults
	// make the run fail while keeping every result for diagnosis.
	for _, r := range results {
		if r.Execution != "" && r.Execution != "ok" {
			return fmt.Errorf("committed execution failure: %s (%s)", r.Hash, r.Execution)
		}
	}
	return nil
}

func preflight(ctx context.Context, client *http.Client, url string, hashes []string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for _, hash := range hashes {
		for {
			var response rpcResponse
			err := rpc(ctx, client, url, request(0, "getTransactionReceipt", hash), &response)
			var r receipt
			if err == nil && (len(response.Error) == 0 || string(response.Error) == "null") && string(response.Result) != "null" && json.Unmarshal(response.Result, &r) == nil && r.Hash == hash {
				if r.Status.Result != "ok" {
					return fmt.Errorf("setup transaction %s failed: %s", hash, r.Status.Result)
				}
				break
			}
			if time.Now().After(deadline) || ctx.Err() != nil {
				return fmt.Errorf("setup receipt unavailable: %s (%v)", hash, err)
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(250 * time.Millisecond):
			}
		}
	}
	return nil
}
func pollReceipts(ctx context.Context, client *http.Client, url string, start time.Time, results []Result, mu *sync.Mutex, deployed map[int]bool, workers int) {
	var group sync.WaitGroup
	for worker := 0; worker < workers; worker++ {
		group.Add(1)
		go func(worker int) {
			defer group.Done()
			pollReceiptShard(ctx, client, url, start, results, mu, deployed, worker, workers)
		}(worker)
	}
	group.Wait()
}

func pollReceiptShard(ctx context.Context, client *http.Client, url string, start time.Time, results []Result, mu *sync.Mutex, deployed map[int]bool, next, stride int) {
	// Each worker owns disjoint transaction indices. Keep a queue of submitted
	// transactions rather than repeatedly scanning millions of future/completed
	// rows under the same mutex used by the arrival scheduler.
	pending, head := []int{}, 0
	deployments, deploymentHead := []int{}, 0
	for ctx.Err() == nil {
		requests := make([]rpcRequest, 0, 128)
		mu.Lock()
		for scanned := 0; next < len(results) && scanned < 4096; scanned++ {
			r := &results[next]
			if r.Outcome == "unsent" {
				break
			}
			if needsReceipt(*r) {
				if r.Kind == "deploy" {
					deployments = append(deployments, next)
				} else {
					pending = append(pending, next)
				}
			}
			next += stride
		}
		// Prioritize dependencies, rotating through them and reserving half the
		// batch for other transactions so unresolved deployments cannot starve it.
		for deploymentHead < len(deployments) && len(requests) < 64 {
			i := deployments[deploymentHead]
			deploymentHead++
			if r := results[i]; needsReceipt(r) {
				requests = append(requests, request(i, "getTransactionReceipt", r.Hash))
			}
		}
		if deploymentHead >= 4096 && deploymentHead >= len(deployments)/2 {
			copy(deployments, deployments[deploymentHead:])
			deployments = deployments[:len(deployments)-deploymentHead]
			deploymentHead = 0
		}
		for head < len(pending) && len(requests) < 128 {
			i := pending[head]
			head++
			r := results[i]
			if needsReceipt(r) {
				requests = append(requests, request(i, "getTransactionReceipt", r.Hash))
			}
		}
		if head >= 4096 && head >= len(pending)/2 {
			copy(pending, pending[head:])
			pending = pending[:len(pending)-head]
			head = 0
		}
		mu.Unlock()
		if len(requests) == 0 {
			select {
			case <-ctx.Done():
				return
			case <-time.After(50 * time.Millisecond):
			}
			continue
		}
		var replies []rpcResponse
		if rpc(ctx, client, url, requests, &replies) == nil {
			mu.Lock()
			for _, reply := range replies {
				if reply.ID < 0 || reply.ID >= len(results) || (len(reply.Error) > 0 && string(reply.Error) != "null") || string(reply.Result) == "null" {
					continue
				}
				var r receipt
				if json.Unmarshal(reply.Result, &r) != nil || r.Hash != results[reply.ID].Hash || r.Status.Result == "" {
					continue
				}
				result := &results[reply.ID]
				if result.ReceiptNs != 0 {
					continue
				}
				result.ReceiptNs = time.Since(start).Nanoseconds()
				result.Height = r.Height
				result.Execution = r.Status.Result
				result.GasUsed = r.GasUsed
				if result.Kind == "deploy" && result.Execution == "ok" {
					deployed[result.App] = true
				}
			}
			mu.Unlock()
		}
		mu.Lock()
		for _, req := range requests {
			r := &results[req.ID]
			if needsReceipt(*r) {
				if r.Kind == "deploy" {
					deployments = append(deployments, req.ID)
				} else {
					pending = append(pending, req.ID)
				}
			}
		}
		mu.Unlock()
		select {
		case <-ctx.Done():
			return
		case <-time.After(10 * time.Millisecond):
		}
	}
}
func scrape(ctx context.Context, client *http.Client, output string, endpoints map[string]string) error {
	file, err := os.Create(filepath.Join(output, "metrics.jsonl"))
	if err != nil {
		return err
	}
	defer file.Close()
	encoder := json.NewEncoder(file)
	successes := map[string]int{}
	for ctx.Err() == nil {
		for name, url := range endpoints {
			record := map[string]any{"name": name, "unixNs": time.Now().UnixNano()}
			req, e := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
			if e == nil {
				response, e := client.Do(req)
				if e == nil {
					body, readErr := io.ReadAll(io.LimitReader(response.Body, 8<<20))
					response.Body.Close()
					if readErr == nil && response.StatusCode == 200 {
						record["text"] = string(body)
						successes[name]++
					} else {
						record["error"] = fmt.Sprint(response.StatusCode, readErr)
					}
				} else {
					record["error"] = e.Error()
				}
			} else {
				record["error"] = e.Error()
			}
			if e = encoder.Encode(record); e != nil {
				return e
			}
		}
		select {
		case <-ctx.Done():
		case <-time.After(time.Second):
		}
	}
	for name := range endpoints {
		if successes[name] < 2 {
			return fmt.Errorf("insufficient metric samples from %s", name)
		}
	}
	return nil
}
