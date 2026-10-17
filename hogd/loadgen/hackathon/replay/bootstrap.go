package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"

	"github.com/mr-tron/base58"
	"github.com/thruputchain/lib-go/librpc"
	"github.com/thruputchain/lib-go/libxdr"
	proto "github.com/thruputchain/thruput-protocol-go"
	xdrrpc "github.com/xdrpp/goxdr/rpc"
)

func hashTransaction(tx *proto.SignedTransaction) proto.TransactionHash {
	h := sha256.New()
	libxdr.MarshalXdrWriter(h, &tx.Tx)
	return proto.TransactionHash(h.Sum(nil))
}

func readBootstrap(corpus string) (proto.LoadgenSuccessResult, error) {
	var result proto.LoadgenSuccessResult
	data, err := os.ReadFile(filepath.Join(corpus, "genesis.xdr"))
	if err != nil {
		return result, err
	}
	err = libxdr.UnmarshalXdrEOF(&result, data)
	return result, err
}

// The unmodified coordinator_loadgen command uses its existing RPC interface
// for chain genesis only. Every application setup transaction goes through the
// public gateway in initializeApps, never through coordinator submission.
type bootstrapService struct{ genesis proto.LoadgenSuccessResult }

func (s *bootstrapService) Generate_experiment(_ context.Context, config proto.ExperimentConfig) (proto.LoadgenResult, error) {
	result := proto.LoadgenResult{Error: proto.KLoadgenRPCErrorInvalidConfig}
	if config.Experiment_type != proto.KExperimentTypeERC20BasicAuthPayments || config.Erc20_payments().Network_hash != s.genesis.Genesis_configuration.Network_hash {
		return result, nil
	}
	result.Error = proto.KLoadgenRPCErrorOK
	*result.Success() = s.genesis
	result.Success().Initialization_txs = nil
	return result, nil
}

func (s *bootstrapService) Extend_experiment(ctx context.Context, id proto.LoadgenContextID, _ uint64, _ proto.BlockHeight) (proto.LoadgenExtensionResult, error) {
	if id != s.genesis.Context_id {
		return proto.LoadgenExtensionResult{Error: proto.KLoadgenRPCErrorInvalidContext}, nil
	}
	// No internally generated work: leave the work stream open so chuckd's
	// normal nomination and commit loop runs until the harness stops the chain.
	<-ctx.Done()
	return proto.LoadgenExtensionResult{}, ctx.Err()
}

func serveBootstrap(ctx context.Context, corpus, address string) error {
	genesis, err := readBootstrap(corpus)
	if err != nil {
		return err
	}
	if len(genesis.Genesis_ceremony_txs) == 0 {
		return fmt.Errorf("corpus has no genesis ceremony")
	}
	listener, err := net.Listen("tcp", address)
	if err != nil {
		return err
	}
	defer listener.Close()
	stop := context.AfterFunc(ctx, func() { listener.Close() })
	defer stop()
	var connections sync.WaitGroup
	defer connections.Wait()
	service := &bootstrapService{genesis: genesis}
	fmt.Println("hackathon genesis server ready")
	for {
		sock, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		driver := librpc.NewDriverSerial(ctx, sock, 0x7fffffff, xdrrpc.DriverRegime{RecvQueueLen: 10, SendQueueLen: 10})
		driver.Register(proto.LoadgenInterfaceV1_Server{Srv: service})
		connections.Add(1)
		go func() {
			defer connections.Done()
			defer driver.Close()
			driver.Go()
		}()
	}
}

func submit(ctx context.Context, client *http.Client, url string, op Operation, id int) (string, error) {
	var reply rpcResponse
	if err := rpc(ctx, client, url, request(id, "sendRawTransaction", op.Raw), &reply); err != nil {
		var status httpStatusError
		if errors.As(err, &status) {
			return "http_error", err
		}
		return "transport_error", err
	}
	if len(reply.Error) > 0 && string(reply.Error) != "null" {
		return "rpc_rejected", fmt.Errorf("%s", reply.Error)
	}
	var hash string
	if err := json.Unmarshal(reply.Result, &hash); err != nil {
		return "transport_error", err
	}
	if hash != op.Hash || reply.ID != id {
		return "transport_error", fmt.Errorf("returned transaction hash or request ID mismatch")
	}
	return "admitted", nil
}

func initializeApps(ctx context.Context, client *http.Client, c Config, manifest Manifest) error {
	genesis, err := readBootstrap(c.Corpus)
	if err != nil {
		return err
	}
	// Verify the entire setup against the manifest before sending any of it.
	var batches [][]Operation
	index := 0
	for _, batch := range genesis.Initialization_txs {
		var ops []Operation
		for _, tx := range batch {
			hash := hashTransaction(&tx)
			op := Operation{Hash: base58.Encode(hash[:])}
			if index >= len(manifest.InitializationHashes) || manifest.InitializationHashes[index] != op.Hash {
				return fmt.Errorf("setup transaction %d does not match manifest", index)
			}
			encoded := libxdr.NewXdrMarshaler().MarshalXdr(&tx)
			op.Raw = "0x" + hex.EncodeToString(encoded.Bytes())
			encoded.Recycle()
			ops = append(ops, op)
			index++
		}
		batches = append(batches, ops)
	}
	if index != len(manifest.InitializationHashes) {
		return fmt.Errorf("setup transaction count does not match manifest")
	}
	file, err := os.Create(filepath.Join(c.Output, "setup.jsonl"))
	if err != nil {
		return err
	}
	defer file.Close()
	encoder := json.NewEncoder(file)
	index = 0
	for _, batch := range batches {
		var hashes []string
		for _, op := range batch {
			outcome, err := submit(ctx, client, c.URL, op, index)
			record := map[string]any{"hash": op.Hash, "outcome": outcome}
			if err != nil {
				record["error"] = err.Error()
			}
			if writeErr := encoder.Encode(record); writeErr != nil {
				return writeErr
			}
			if outcome == "rpc_rejected" {
				return fmt.Errorf("setup submission rejected: %s: %w", op.Hash, err)
			}
			hashes = append(hashes, op.Hash)
			index++
		}
		// An ambiguous HTTP timeout is resolved by receipts, without resending.
		// Commit each dependency batch before proceeding to dependent calls.
		if err := preflight(ctx, client, c.URL, hashes, c.Drain); err != nil {
			return err
		}
	}
	fmt.Printf("hackathon setup committed through RPC gateway: %d transactions\n", index)
	return nil
}
