package app

import (
	"fmt"
	"os"
	"time"
)

// Mode represents the process role.
type Mode string

const (
	ModeMaster Mode = "master"
	ModeSlave  Mode = "slave"
)

// Config holds runtime settings for the Go-based autod-lite daemon.
type Config struct {
	ID               string        `yaml:"id"`
	Mode             Mode          `yaml:"mode"`
	Listen           string        `yaml:"listen"`
	Advertise        string        `yaml:"advertise"`
	MasterURL        string        `yaml:"master_url"`
	ExecTimeout      time.Duration `yaml:"exec_timeout"`
	RegisterInterval time.Duration `yaml:"register_interval"`
	ProbeCIDRs       []string      `yaml:"probe_cidrs"`
	ProbeInterval    time.Duration `yaml:"probe_interval"`
	ProbePort        int           `yaml:"probe_port"`
	Slots            []string      `yaml:"slots"`
}

// DefaultConfig returns sensible defaults for missing configuration values.
func DefaultConfig() Config {
	return Config{
		Listen:           ":8080",
		ExecTimeout:      10 * time.Second,
		RegisterInterval: 15 * time.Second,
		ProbeInterval:    45 * time.Second,
		ProbePort:        8080,
	}
}

func generateNodeID() string {
	host, err := os.Hostname()
	if err != nil || host == "" {
		host = "autod-node"
	}
	return fmt.Sprintf("%s-%d", host, time.Now().UnixNano())
}

// applyCommonDefaults normalizes zero values after load.
func applyCommonDefaults(cfg Config) Config {
	defaults := DefaultConfig()
	if cfg.Listen == "" {
		cfg.Listen = defaults.Listen
	}
	if cfg.ExecTimeout <= 0 {
		cfg.ExecTimeout = defaults.ExecTimeout
	}
	if cfg.RegisterInterval <= 0 {
		cfg.RegisterInterval = defaults.RegisterInterval
	}
	if cfg.ProbeInterval <= 0 {
		cfg.ProbeInterval = defaults.ProbeInterval
	}
	if cfg.ProbePort == 0 {
		cfg.ProbePort = defaults.ProbePort
	}
	return cfg
}

// validateMode ensures Mode is present and known.
func validateMode(mode Mode) error {
	if mode != ModeMaster && mode != ModeSlave {
		return fmt.Errorf("mode must be 'master' or 'slave'")
	}
	return nil
}
