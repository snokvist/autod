package main

import (
	"context"
	"flag"
	"log"
	"os"
	"strings"

	"autodlite/internal/app"
)

func main() {
	cfgPath := flag.String("config", "", "path to YAML config")
	flag.Parse()

	cfg, err := app.LoadConfig(*cfgPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	logger := log.New(os.Stdout, "autod-lite ", log.LstdFlags)
	registry := app.NewRegistry()

	// Seed master with itself for visibility.
	if cfg.Mode == app.ModeMaster {
		registry.UpsertNode(app.Node{
			ID:       cfg.ID,
			Address:  normalizeAddress(cfg.Listen),
			Role:     cfg.Mode,
			Slots:    cfg.Slots,
			LastSeen: app.NowUTC(),
			Healthy:  true,
			Source:   "self",
		})
	}

	server := app.NewServer(cfg, registry, logger)
	if err := server.Run(context.Background()); err != nil {
		log.Fatalf("server stopped: %v", err)
	}
}

func normalizeAddress(listen string) string {
	if strings.HasPrefix(listen, ":") {
		return "127.0.0.1" + listen
	}
	return listen
}
