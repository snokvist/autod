//go:build tiny

package app

import (
        "os"
        "strconv"
        "strings"
        "time"
)

// LoadConfig provides an env-only config path for size-constrained builds.
// Expected env vars (fallback to defaults where noted):
//
//	AUTOD_MODE          (required: master|slave)
//	AUTOD_LISTEN        (default :8080)
//	AUTOD_ADVERTISE     (optional override advertised host:port)
//	AUTOD_MASTER_URL    (for slave registration)
//	AUTOD_EXEC_TIMEOUT  (default 10s)
//	AUTOD_REGISTER_EVERY (default 15s)
//	AUTOD_PROBE_CIDRS   (comma-separated list)
//	AUTOD_PROBE_EVERY   (default 45s)
//	AUTOD_PROBE_PORT    (default 8080)
//	AUTOD_SLOTS         (comma-separated list)
//	AUTOD_ID            (optional explicit ID)
func LoadConfig(_ string) (Config, error) {
	cfg := DefaultConfig()

	if v := strings.TrimSpace(os.Getenv("AUTOD_ID")); v != "" {
		cfg.ID = v
	} else {
		cfg.ID = generateNodeID()
	}

	cfg.Mode = Mode(strings.TrimSpace(os.Getenv("AUTOD_MODE")))
	if err := validateMode(cfg.Mode); err != nil {
		return cfg, err
	}

	if v := strings.TrimSpace(os.Getenv("AUTOD_LISTEN")); v != "" {
		cfg.Listen = v
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_ADVERTISE")); v != "" {
		cfg.Advertise = v
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_MASTER_URL")); v != "" {
		cfg.MasterURL = v
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_EXEC_TIMEOUT")); v != "" {
		if parsed, err := time.ParseDuration(v); err == nil {
			cfg.ExecTimeout = parsed
		}
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_REGISTER_EVERY")); v != "" {
		if parsed, err := time.ParseDuration(v); err == nil {
			cfg.RegisterInterval = parsed
		}
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_PROBE_EVERY")); v != "" {
		if parsed, err := time.ParseDuration(v); err == nil {
			cfg.ProbeInterval = parsed
		}
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_PROBE_PORT")); v != "" {
		if parsed, err := strconv.Atoi(v); err == nil {
			cfg.ProbePort = parsed
		}
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_PROBE_CIDRS")); v != "" {
		cfg.ProbeCIDRs = splitAndTrim(v)
	}
	if v := strings.TrimSpace(os.Getenv("AUTOD_SLOTS")); v != "" {
		cfg.Slots = splitAndTrim(v)
	}

	cfg = applyCommonDefaults(cfg)
	return cfg, nil
}

func splitAndTrim(raw string) []string {
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		if trimmed := strings.TrimSpace(p); trimmed != "" {
			out = append(out, trimmed)
		}
	}
	return out
}
