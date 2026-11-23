package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/mr-tron/base58"
	"github.com/thruputchain/lib-go/libxdr"
	proto "github.com/thruputchain/thruput-protocol-go"
)

func TestBootstrapSuppliesOnlyGenesisAndKeepsWorkStreamOpen(t *testing.T) {
	genesis := proto.LoadgenSuccessResult{Context_id: 19}
	genesis.Genesis_configuration.Network_hash[0] = 7
	genesis.Genesis_ceremony_txs = []proto.SignedTransaction{{}}
	genesis.Initialization_txs = []proto.InitializationBlock{{{}}}
	service := bootstrapService{genesis: genesis}
	config := proto.ExperimentConfig{Experiment_type: proto.KExperimentTypeERC20BasicAuthPayments}
	config.Erc20_payments().Network_hash = genesis.Genesis_configuration.Network_hash
	result, err := service.Generate_experiment(context.Background(), config)
	if err != nil || result.Error != proto.KLoadgenRPCErrorOK {
		t.Fatalf("generate: %v, %v", result.Error, err)
	}
	if len(result.Success().Initialization_txs) != 0 || len(result.Success().Genesis_ceremony_txs) != 1 || len(service.genesis.Initialization_txs) != 1 {
		t.Fatal("bootstrap must expose genesis only, preserving setup for HTTP replay")
	}
	config.Erc20_payments().Network_hash[0]++
	result, err = service.Generate_experiment(context.Background(), config)
	if err != nil || result.Error != proto.KLoadgenRPCErrorInvalidConfig {
		t.Fatal("accepted mismatched network")
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		_, err := service.Extend_experiment(ctx, 19, 100, 1000)
		done <- err
	}()
	select {
	case <-done:
		t.Fatal("work stream ended before shutdown")
	case <-time.After(20 * time.Millisecond):
	}
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("shutdown: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("work stream failed to stop")
	}
}

func TestSetupUsesGatewayAndWaitsForDependencyReceipts(t *testing.T) {
	directory := t.TempDir()
	genesis := proto.LoadgenSuccessResult{Genesis_configuration: proto.GenesisConfiguration{Root_contract: proto.Contract{Contract_type: proto.KRv32}}}
	manifest := Manifest{}
	rawToHash := map[string]string{}
	for i := 0; i < 2; i++ {
		tx := proto.SignedTransaction{}
		tx.Tx.Uniquifier.Nonce = uint64(i)
		genesis.Initialization_txs = append(genesis.Initialization_txs, []proto.SignedTransaction{tx})
		hash := hashTransaction(&tx)
		key := base58.Encode(hash[:])
		manifest.InitializationHashes = append(manifest.InitializationHashes, key)
		encoded := libxdr.NewXdrMarshaler().MarshalXdr(&tx)
		rawToHash["0x"+hex.EncodeToString(encoded.Bytes())] = key
		encoded.Recycle()
	}
	encoded := libxdr.NewXdrMarshaler().MarshalXdr(&genesis)
	defer encoded.Recycle()
	if err := os.WriteFile(filepath.Join(directory, "genesis.xdr"), encoded.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	var requests []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var req rpcRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Error(err)
		}
		requests = append(requests, req.Method)
		var result any
		if req.Method == "groundhog_sendRawTransaction" {
			result = rawToHash[req.Params[0]]
		} else {
			result = map[string]any{"txHash": req.Params[0], "status": map[string]string{"result": "ok"}}
		}
		json.NewEncoder(w).Encode(map[string]any{"id": req.ID, "result": result})
	}))
	c := Config{Corpus: directory, Output: directory, URL: server.URL, Drain: time.Second}
	err := initializeApps(context.Background(), server.Client(), c, manifest)
	server.Close()
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"groundhog_sendRawTransaction", "groundhog_getTransactionReceipt", "groundhog_sendRawTransaction", "groundhog_getTransactionReceipt"}
	if len(requests) != len(want) {
		t.Fatalf("unexpected gateway requests: %v", requests)
	}
	for i := range want {
		if requests[i] != want[i] {
			t.Fatalf("setup did not wait for dependency commitment: %v", requests)
		}
	}
	// A mismatched setup must fail before making further network requests.
	manifest.InitializationHashes[0] = "wrong"
	if err := initializeApps(context.Background(), server.Client(), c, manifest); err == nil {
		t.Fatal("accepted setup inconsistent with manifest")
	}
}
