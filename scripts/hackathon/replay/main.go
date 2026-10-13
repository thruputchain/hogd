package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func main() {
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	if err := command(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func command(ctx context.Context, args []string) error {
	if len(args) == 0 || (args[0] != "run" && args[0] != "bootstrap") {
		return fmt.Errorf("usage: hackathon-replay <run|bootstrap> --corpus DIR [options]")
	}
	f := flag.NewFlagSet(args[0], flag.ContinueOnError)
	var c Config
	f.StringVar(&c.Corpus, "corpus", "", "saved corpus directory")
	if args[0] == "bootstrap" {
		listen := f.String("listen", "127.0.0.1:44400", "genesis-only loadgen RPC listen address")
		if err := f.Parse(args[1:]); err != nil {
			return err
		}
		if c.Corpus == "" || f.NArg() != 0 {
			return fmt.Errorf("bootstrap requires --corpus and no positional arguments")
		}
		return serveBootstrap(ctx, c.Corpus, *listen)
	}
	f.StringVar(&c.Output, "output", "", "new result directory")
	f.StringVar(&c.URL, "url", "http://127.0.0.1:9546", "RPC gateway URL for setup, submissions, and receipts")
	f.IntVar(&c.Outstanding, "outstanding", 2048, "maximum concurrent submissions")
	f.DurationVar(&c.Drain, "drain", 120*time.Second, "receipt drain timeout")
	f.DurationVar(&c.Timeout, "request-timeout", 15*time.Second, "request timeout")
	f.DurationVar(&c.MaxLateness, "max-lateness", 100*time.Millisecond, "drop arrivals delayed beyond this")
	metrics := f.String("metrics", "", "comma-separated name=metrics-URL pairs")
	if err := f.Parse(args[1:]); err != nil {
		return err
	}
	if c.Corpus == "" || c.Output == "" || f.NArg() != 0 {
		return fmt.Errorf("run requires --corpus, --output, and no positional arguments")
	}
	c.Metrics = map[string]string{}
	if *metrics != "" {
		for _, endpoint := range strings.Split(*metrics, ",") {
			name, url, ok := strings.Cut(endpoint, "=")
			if !ok || name == "" || url == "" {
				return fmt.Errorf("invalid metrics endpoint %q", endpoint)
			}
			c.Metrics[name] = url
		}
	}
	return Run(ctx, c)
}
