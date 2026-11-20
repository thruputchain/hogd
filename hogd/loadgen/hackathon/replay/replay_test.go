package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/mr-tron/base58"
	"github.com/thruputchain/lib-go/libxdr"
	proto "github.com/thruputchain/thruput-protocol-go"
)

func fixture(t *testing.T, n int) (string, []Operation) {
	t.Helper()
	dir := t.TempDir()
	ops := make([]Operation, n)
	f, err := os.Create(filepath.Join(dir, "transactions.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	for i := range ops {
		tx := proto.SignedTransaction{}
		tx.Tx.Uniquifier.Nonce = uint64(i)
		hash := hashTransaction(&tx)
		raw := libxdr.NewXdrMarshaler().MarshalXdr(proto.XDR_SignedTransaction(&tx)).Bytes()
		ops[i] = Operation{DueNs: int64(i) * int64(time.Millisecond), Phase: 1, Kind: "call", Dependency: -1, Hash: base58.Encode(hash[:]), Raw: "0x" + hex.EncodeToString(raw)}
		if err = json.NewEncoder(f).Encode(ops[i]); err != nil {
			t.Fatal(err)
		}
	}
	data, _ := json.Marshal(Manifest{SchemaVersion: 2, Operations: n, DurationSeconds: 1})
	if err = os.WriteFile(filepath.Join(dir, "manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}
	genesis := libxdr.NewXdrMarshaler().MarshalXdr(&proto.LoadgenSuccessResult{Genesis_configuration: proto.GenesisConfiguration{Root_contract: proto.Contract{Contract_type: proto.KRv32}}})
	defer genesis.Recycle()
	if err = os.WriteFile(filepath.Join(dir, "genesis.xdr"), genesis.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	return dir, ops
}
func TestCorpusValidation(t *testing.T) {
	dir, _ := fixture(t, 3)
	if _, ops, err := load(Config{Corpus: dir}); err != nil || len(ops) != 3 {
		t.Fatalf("load: %v", err)
	}
	path := filepath.Join(dir, "transactions.jsonl")
	data, _ := os.ReadFile(path)
	var rows []Operation
	for _, line := range bytesLines(data) {
		var op Operation
		json.Unmarshal(line, &op)
		rows = append(rows, op)
	}
	rows[1].Hash = rows[0].Hash
	file, _ := os.Create(path)
	for _, op := range rows {
		json.NewEncoder(file).Encode(op)
	}
	file.Close()
	if _, _, err := load(Config{Corpus: dir}); err == nil {
		t.Fatal("accepted mismatched transaction hash")
	}
}
func TestCorpusRejectsLegacyHash(t *testing.T) {
	dir, ops := fixture(t, 1)
	hash, err := base58.Decode(ops[0].Hash)
	if err != nil {
		t.Fatal(err)
	}
	ops[0].Hash = "0x" + hex.EncodeToString(hash)
	data, err := json.Marshal(ops[0])
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "transactions.jsonl"), append(data, '\n'), 0644); err != nil {
		t.Fatal(err)
	}
	if _, _, err := load(Config{Corpus: dir}); err == nil {
		t.Fatal("accepted a legacy hex transaction hash")
	}
}

func bytesLines(data []byte) [][]byte {
	var lines [][]byte
	start := 0
	for i, b := range data {
		if b == '\n' {
			lines = append(lines, data[start:i])
			start = i + 1
		}
	}
	return lines
}

func TestSlowRPCDoesNotThrottleOfferedArrivals(t *testing.T) {
	dir, ops := fixture(t, 40)
	var mu sync.Mutex
	committed := map[string]bool{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body json.RawMessage
		json.NewDecoder(r.Body).Decode(&body)
		reply := func(req rpcRequest) rpcResponse {
			if req.Method == "groundhog_sendRawTransaction" {
				var hash string
				for _, op := range ops {
					if op.Raw == req.Params[0] {
						hash = op.Hash
						break
					}
				}
				time.Sleep(80 * time.Millisecond)
				mu.Lock()
				committed[hash] = true
				mu.Unlock()
				data, _ := json.Marshal(hash)
				return rpcResponse{ID: req.ID, Result: data}
			}
			mu.Lock()
			ok := committed[req.Params[0]]
			mu.Unlock()
			data := json.RawMessage("null")
			if ok {
				data = json.RawMessage(fmt.Sprintf(`{"txHash":%q,"height":"2","status":{"result":"ok"},"gasUsed":"100"}`, req.Params[0]))
			}
			return rpcResponse{ID: req.ID, Result: data}
		}
		if len(body) > 0 && body[0] == '[' {
			var requests []rpcRequest
			json.Unmarshal(body, &requests)
			responses := []rpcResponse{}
			for _, req := range requests {
				responses = append(responses, reply(req))
			}
			json.NewEncoder(w).Encode(responses)
		} else {
			var req rpcRequest
			json.Unmarshal(body, &req)
			json.NewEncoder(w).Encode(reply(req))
		}
	}))
	defer server.Close()
	out := filepath.Join(t.TempDir(), "run")
	c := Config{Corpus: dir, Output: out, URL: server.URL, Outstanding: 1, ReceiptWorkers: 4, Drain: time.Second, Timeout: time.Second, MaxLateness: time.Second}
	if err := Run(context.Background(), c); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(filepath.Join(out, "results.jsonl"))
	blocked, admitted := 0, 0
	for _, line := range bytesLines(data) {
		var r Result
		json.Unmarshal(line, &r)
		if r.Outcome == "outstanding_limit" {
			blocked++
		}
		if r.Outcome == "admitted" {
			admitted++
			if r.Execution != "ok" {
				t.Fatal("receipt not reconciled")
			}
		}
	}
	if blocked == 0 || admitted == 0 || blocked+admitted != len(ops) {
		t.Fatalf("incomplete accounting: %d blocked %d admitted", blocked, admitted)
	}
}
func TestPreflightRejectsCommittedFault(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"id":0,"result":{"txHash":"0x01","status":{"result":"deterministicError"}}}`)
	}))
	defer server.Close()
	if err := preflight(context.Background(), server.Client(), server.URL, []string{"0x01"}, time.Second); err == nil {
		t.Fatal("setup accepted committed fault")
	}
}
func TestConfigurationRejectedBeforeCreatingOutput(t *testing.T) {
	if err := Run(context.Background(), Config{}); err == nil {
		t.Fatal("accepted invalid replay configuration")
	}
}

func TestHTTPRejectionsAndAmbiguousFailures(t *testing.T) {
	for _, code := range []int{429, 408, 500} {
		t.Run(fmt.Sprint(code), func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(code)
			}))
			defer server.Close()
			outcome, err := submit(context.Background(), server.Client(), server.URL, Operation{}, 0)
			var status httpStatusError
			if outcome != "http_error" || !errors.As(err, &status) || status.code != code {
				t.Fatalf("HTTP failure misclassified: %s %v", outcome, err)
			}
			result := Result{StartNs: 1, Outcome: outcome, HTTPStatus: status.code}
			if got, want := needsReceipt(result), code != 429; got != want {
				t.Fatalf("receipt eligibility: got %v, want %v", got, want)
			}
		})
	}
}

func TestAmbiguousSubmissionIsReconciledByReceipt(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var requests []rpcRequest
		if err := json.NewDecoder(r.Body).Decode(&requests); err != nil {
			t.Error(err)
		}
		for _, req := range requests {
			if req.ID != 0 {
				t.Error("polled a transaction rejected with HTTP 429")
			}
		}
		fmt.Fprint(w, `[{"id":0,"error":null,"result":{"txHash":"0x01","height":"3","status":{"result":"ok"},"gasUsed":"120"}}]`)
	}))
	defer server.Close()
	results := []Result{
		{Operation: Operation{Hash: "0x01", Kind: "deploy", App: 7}, StartNs: 1, Outcome: "transport_error"},
		{Operation: Operation{Hash: "0x02", Kind: "call"}, StartNs: 1, Outcome: "http_error", HTTPStatus: 429},
	}
	var mu sync.Mutex
	deployed := map[int]bool{}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	done := make(chan struct{})
	go func() {
		pollReceipts(ctx, server.Client(), server.URL, time.Now(), results, &mu, deployed, 4)
		close(done)
	}()
	for {
		mu.Lock()
		committed := deployed[7]
		mu.Unlock()
		if committed {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatal("receipt not reconciled")
		case <-time.After(time.Millisecond):
		}
	}
	cancel()
	<-done
	if results[0].Outcome != "transport_error" || results[0].Execution != "ok" {
		t.Fatal("transport and execution outcomes must remain separate")
	}
}

func TestDependencyBlockedArrivalsAreAccounted(t *testing.T) {
	dir, ops := fixture(t, 1)
	ops[0].Dependency = 77
	file, err := os.Create(filepath.Join(dir, "transactions.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	json.NewEncoder(file).Encode(ops[0])
	file.Close()
	out := filepath.Join(t.TempDir(), "run")
	c := Config{Corpus: dir, Output: out, URL: "http://unused.invalid", Outstanding: 1, ReceiptWorkers: 4, Drain: time.Second, Timeout: time.Second, MaxLateness: time.Second}
	if err = Run(context.Background(), c); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(filepath.Join(out, "results.jsonl"))
	var result Result
	if err = json.Unmarshal(data, &result); err != nil {
		t.Fatal(err)
	}
	if result.Outcome != "dependency_blocked" || result.StartNs != 0 {
		t.Fatalf("unexpected result: %+v", result)
	}
}

func TestPendingDeploymentsDoNotStarveLaterReceipts(t *testing.T) {
	const deployments = 260
	results := make([]Result, deployments+1)
	for i := range results {
		kind := "deploy"
		if i == deployments {
			kind = "call"
		}
		results[i] = Result{Operation: Operation{Hash: fmt.Sprintf("0x%04x", i), Kind: kind, App: i}, StartNs: 1, Outcome: "admitted"}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	firstBatch := true
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var batch []rpcRequest
		if err := json.NewDecoder(r.Body).Decode(&batch); err != nil {
			t.Error(err)
			return
		}
		responses := make([]rpcResponse, 0, len(batch))
		includesCall := false
		for _, req := range batch {
			includesCall = includesCall || req.ID == deployments
			result := json.RawMessage("null")
			// Leave the first 128 deployments unresolved forever.
			if req.ID >= 128 {
				result = json.RawMessage(fmt.Sprintf(`{"txHash":%q,"height":"3","status":{"result":"ok"}}`, req.Params[0]))
			}
			responses = append(responses, rpcResponse{ID: req.ID, Result: result})
		}
		if firstBatch && !includesCall {
			t.Error("deployment polling did not reserve space for calls")
		}
		firstBatch = false
		json.NewEncoder(w).Encode(responses)
	}))
	defer server.Close()
	var mu sync.Mutex
	deployed := map[int]bool{}
	done := make(chan struct{})
	go func() {
		pollReceipts(ctx, server.Client(), server.URL, time.Now(), results, &mu, deployed, 1)
		close(done)
	}()
	defer func() { cancel(); <-done }()
	for {
		mu.Lock()
		callComplete := results[deployments].ReceiptNs > 0
		deployComplete := len(deployed) == deployments-128
		mu.Unlock()
		if callComplete && deployComplete {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatalf("receipts starved: call complete=%v, later deployments complete=%v", callComplete, deployComplete)
		case <-time.After(time.Millisecond):
		}
	}
}

func TestReceiptWorkersPollDisjointBatchesConcurrently(t *testing.T) {
	const workers = 4
	results := make([]Result, workers*128+1)
	for i := range results {
		results[i] = Result{Operation: Operation{Hash: fmt.Sprintf("0x%04x", i), Kind: "call"}, StartNs: 1, Outcome: "admitted"}
	}
	// A future transaction must remain unpolled until it is submitted.
	results[len(results)-1].StartNs = 0
	results[len(results)-1].Outcome = "unsent"
	arrived, release := make(chan struct{}, workers), make(chan struct{})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var batch []rpcRequest
		if err := json.NewDecoder(r.Body).Decode(&batch); err != nil {
			t.Error(err)
			return
		}
		select {
		case arrived <- struct{}{}:
		default:
		}
		select {
		case <-release:
		case <-ctx.Done():
			return
		}
		replies := make([]rpcResponse, 0, len(batch))
		for _, req := range batch {
			if req.ID == len(results)-1 {
				t.Error("polled an unsubmitted transaction")
			}
			replies = append(replies, rpcResponse{ID: req.ID, Result: json.RawMessage(fmt.Sprintf(`{"txHash":%q,"height":"3","status":{"result":"ok"}}`, req.Params[0]))})
		}
		json.NewEncoder(w).Encode(replies)
	}))
	defer server.Close()
	defer cancel()
	var mu sync.Mutex
	done := make(chan struct{})
	go func() {
		pollReceipts(ctx, server.Client(), server.URL, time.Now(), results, &mu, map[int]bool{}, workers)
		close(done)
	}()
	for i := 0; i < workers; i++ {
		select {
		case <-arrived:
		case <-ctx.Done():
			t.Fatal("receipt requests did not overlap across workers")
		}
	}
	close(release)
	for {
		mu.Lock()
		complete := 0
		for _, result := range results {
			if result.ReceiptNs > 0 {
				complete++
			}
		}
		mu.Unlock()
		if complete == workers*128 {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatalf("only reconciled %d receipts", complete)
		case <-time.After(time.Millisecond):
		}
	}
	cancel()
	<-done
}
