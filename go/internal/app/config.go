//go:build !tiny

package app

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// LoadConfig reads the provided YAML file into a Config and applies defaults.
func LoadConfig(path string) (Config, error) {
	cfg := DefaultConfig()
	if path == "" {
		return cfg, fmt.Errorf("config path is required (or build with -tags tiny for env-based config)")
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
	if err := validateMode(cfg.Mode); err != nil {
		return cfg, err
	}
	cfg = applyCommonDefaults(cfg)
	return cfg, nil
}
