package app

import (
	"fmt"
	"os"
	"time"

	"gopkg.in/yaml.v3"
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

// LoadConfig reads the provided YAML file into a Config and applies defaults.
func LoadConfig(path string) (Config, error) {
	cfg := DefaultConfig()
	if path == "" {
		return cfg, fmt.Errorf("config path is required")
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, fmt.Errorf("read config: %w", err)
	}
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return cfg, fmt.Errorf("parse config: %w", err)
	}
	if cfg.ID == "" {
		cfg.ID = generateNodeID()
	}
	if cfg.Mode != ModeMaster && cfg.Mode != ModeSlave {
		return cfg, fmt.Errorf("mode must be 'master' or 'slave'")
	}
	if cfg.Listen == "" {
		cfg.Listen = DefaultConfig().Listen
	}
	if cfg.ExecTimeout <= 0 {
		cfg.ExecTimeout = DefaultConfig().ExecTimeout
	}
	if cfg.RegisterInterval <= 0 {
		cfg.RegisterInterval = DefaultConfig().RegisterInterval
	}
	if cfg.ProbeInterval <= 0 {
		cfg.ProbeInterval = DefaultConfig().ProbeInterval
	}
	if cfg.ProbePort == 0 {
		cfg.ProbePort = DefaultConfig().ProbePort
	}
	return cfg, nil
}

func generateNodeID() string {
	host, err := os.Hostname()
	if err != nil || host == "" {
		host = "autod-node"
	}
	return fmt.Sprintf("%s-%d", host, time.Now().UnixNano())
}
