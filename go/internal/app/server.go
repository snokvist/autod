package app

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os/exec"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Server exposes HTTP handlers for health, exec, and sync flows.
type Server struct {
	cfg        Config
	registry   *Registry
	httpClient *http.Client
	logger     *log.Logger
}

// NewServer constructs a Server.
func NewServer(cfg Config, registry *Registry, logger *log.Logger) *Server {
	return &Server{
		cfg:      cfg,
		registry: registry,
		httpClient: &http.Client{
			Timeout: 5 * time.Second,
		},
		logger: logger,
	}
}

// Run launches the HTTP server and background routines.
func (s *Server) Run(ctx context.Context) error {
	srv := &http.Server{
		Addr:    s.cfg.Listen,
		Handler: s.routes(),
	}

	ctx, stop := signal.NotifyContext(ctx, syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	var wg sync.WaitGroup

	if s.cfg.Mode == ModeSlave && s.cfg.MasterURL != "" {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s.registerLoop(ctx)
		}()
	}

	if s.cfg.Mode == ModeMaster && len(s.cfg.ProbeCIDRs) > 0 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s.probeLoop(ctx)
		}()
	}

	wg.Add(1)
	go func() {
		defer wg.Done()
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx)
	}()

	s.logger.Printf("starting %s on %s (id=%s)", s.cfg.Mode, s.cfg.Listen, s.cfg.ID)
	if err := srv.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
		return err
	}

	wg.Wait()
	return nil
}

func (s *Server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", s.handleHealth)
	mux.HandleFunc("/exec", s.handleExec)
	mux.HandleFunc("/nodes", s.handleNodes)
	mux.HandleFunc("/sync/register", s.handleRegister)
	mux.HandleFunc("/sync/slots", s.handleSlots)
	mux.HandleFunc("/sync/slots/", s.handleSlotAction)
	return mux
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	payload := map[string]any{
		"id":        s.cfg.ID,
		"role":      s.cfg.Mode,
		"slots":     s.cfg.Slots,
		"timestamp": time.Now().UTC().Format(time.RFC3339Nano),
	}
	s.writeJSON(w, payload)
}

func (s *Server) handleExec(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Command string   `json:"command"`
		Args    []string `json:"args"`
		Timeout string   `json:"timeout"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid payload", http.StatusBadRequest)
		return
	}
	if req.Command == "" {
		http.Error(w, "command is required", http.StatusBadRequest)
		return
	}
	timeout := s.cfg.ExecTimeout
	if req.Timeout != "" {
		if parsed, err := time.ParseDuration(req.Timeout); err == nil && parsed > 0 {
			timeout = parsed
		}
	}

	ctx, cancel := context.WithTimeout(r.Context(), timeout)
	defer cancel()

	cmd := exec.CommandContext(ctx, req.Command, req.Args...) // #nosec G204 -- command is provided intentionally
	output, err := cmd.CombinedOutput()
	resp := map[string]any{
		"command": req.Command,
		"args":    req.Args,
		"output":  string(output),
		"timeout": timeout.String(),
	}
	if err != nil {
		resp["error"] = err.Error()
	}
	s.writeJSON(w, resp)
}

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Mode != ModeMaster {
		http.Error(w, "registration only available on master", http.StatusBadRequest)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var payload struct {
		ID      string   `json:"id"`
		Address string   `json:"address"`
		Slots   []string `json:"slots"`
		Role    Mode     `json:"role"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "invalid payload", http.StatusBadRequest)
		return
	}
	if payload.ID == "" || payload.Address == "" {
		http.Error(w, "id and address required", http.StatusBadRequest)
		return
	}
	if payload.Role == "" {
		payload.Role = ModeSlave
	}
	s.registry.UpsertNode(Node{
		ID:       payload.ID,
		Address:  payload.Address,
		Role:     payload.Role,
		Slots:    payload.Slots,
		LastSeen: time.Now().UTC(),
		Healthy:  true,
		Source:   "register",
	})
	s.writeJSON(w, map[string]any{"status": "ok"})
}

func (s *Server) handleNodes(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	s.writeJSON(w, map[string]any{"nodes": s.registry.AllNodes()})
}

func (s *Server) handleSlots(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Mode != ModeMaster {
		http.Error(w, "slots only available on master", http.StatusBadRequest)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	s.writeJSON(w, map[string]any{"slots": s.registry.SlotMap()})
}

func (s *Server) handleSlotAction(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Mode != ModeMaster {
		http.Error(w, "slot actions only available on master", http.StatusBadRequest)
		return
	}

	path := strings.TrimPrefix(r.URL.Path, "/sync/slots/")
	if path == "" {
		http.NotFound(w, r)
		return
	}

	parts := strings.Split(path, "/")
	slot := parts[0]

	switch r.Method {
	case http.MethodPut:
		var req struct {
			NodeID string `json:"node_id"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.NodeID == "" {
			http.Error(w, "node_id required", http.StatusBadRequest)
			return
		}
		if _, ok := s.registry.GetNode(req.NodeID); !ok {
			http.Error(w, "unknown node", http.StatusNotFound)
			return
		}
		s.registry.SetSlotBinding(slot, req.NodeID)
		s.writeJSON(w, map[string]any{"slot": slot, "node_id": req.NodeID})
	case http.MethodPost:
		if len(parts) < 2 || parts[1] != "exec" {
			http.NotFound(w, r)
			return
		}
		s.handleSlotExec(w, r, slot)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleSlotExec(w http.ResponseWriter, r *http.Request, slot string) {
	nodeID, ok := s.registry.SlotBinding(slot)
	if !ok {
		http.Error(w, "slot not assigned", http.StatusNotFound)
		return
	}
	node, ok := s.registry.GetNode(nodeID)
	if !ok {
		http.Error(w, "node not found", http.StatusNotFound)
		return
	}

	var payload map[string]any
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "invalid payload", http.StatusBadRequest)
		return
	}

	target := fmt.Sprintf("http://%s/exec", node.Address)
	body, _ := json.Marshal(payload)
	req, err := http.NewRequestWithContext(r.Context(), http.MethodPost, target, bytes.NewReader(body))
	if err != nil {
		http.Error(w, "request build failed", http.StatusInternalServerError)
		return
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := s.httpClient.Do(req)
	if err != nil {
		http.Error(w, fmt.Sprintf("dispatch failed: %v", err), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(resp.StatusCode)
	limited := http.MaxBytesReader(w, resp.Body, 2<<20)
	defer limited.Close()
	_, _ = io.Copy(w, limited)
}

func (s *Server) writeJSON(w http.ResponseWriter, payload any) {
	w.Header().Set("Content-Type", "application/json")
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	_ = enc.Encode(payload)
}

// registerLoop periodically registers the node with the master.
func (s *Server) registerLoop(ctx context.Context) {
	ticker := time.NewTicker(s.cfg.RegisterInterval)
	defer ticker.Stop()

	for {
		s.pushRegistration(ctx)
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (s *Server) pushRegistration(ctx context.Context) {
	if s.cfg.MasterURL == "" || s.cfg.Advertise == "" {
		s.logger.Println("skip registration: master_url or advertise unset")
		return
	}
	payload := map[string]any{
		"id":      s.cfg.ID,
		"address": s.cfg.Advertise,
		"slots":   s.cfg.Slots,
		"role":    s.cfg.Mode,
	}
	body, _ := json.Marshal(payload)

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, strings.TrimRight(s.cfg.MasterURL, "/")+"/sync/register", bytes.NewReader(body))
	if err != nil {
		s.logger.Printf("build registration: %v", err)
		return
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := s.httpClient.Do(req)
	if err != nil {
		s.logger.Printf("register failed: %v", err)
		return
	}
	resp.Body.Close()
}

// probeLoop discovers nodes by scanning configured CIDRs for /health.
func (s *Server) probeLoop(ctx context.Context) {
	ticker := time.NewTicker(s.cfg.ProbeInterval)
	defer ticker.Stop()

	for {
		s.scanOnce(ctx)
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (s *Server) scanOnce(ctx context.Context) {
	for _, cidr := range s.cfg.ProbeCIDRs {
		hosts, err := hostsFromCIDR(cidr)
		if err != nil {
			s.logger.Printf("probe cidr %s: %v", cidr, err)
			continue
		}
		for _, ip := range hosts {
			select {
			case <-ctx.Done():
				return
			default:
			}
			addr := net.JoinHostPort(ip.String(), fmt.Sprintf("%d", s.cfg.ProbePort))
			s.queryHealth(ctx, addr)
		}
	}
}

func (s *Server) queryHealth(ctx context.Context, hostport string) {
	url := fmt.Sprintf("http://%s/health", hostport)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return
	}
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return
	}
	var payload struct {
		ID    string   `json:"id"`
		Role  Mode     `json:"role"`
		Slots []string `json:"slots"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&payload); err != nil {
		return
	}
	if payload.ID == "" {
		return
	}
	s.registry.UpsertNode(Node{
		ID:       payload.ID,
		Address:  hostport,
		Role:     payload.Role,
		Slots:    payload.Slots,
		LastSeen: time.Now().UTC(),
		Healthy:  true,
		Source:   "probe",
	})
}

// hostsFromCIDR returns all host IPs within a CIDR, skipping network/broadcast when possible.
func hostsFromCIDR(cidr string) ([]net.IP, error) {
	_, ipnet, err := net.ParseCIDR(cidr)
	if err != nil {
		return nil, err
	}
	var ips []net.IP
	for ip := ipnet.IP.Mask(ipnet.Mask); ipnet.Contains(ip); ip = incrementIP(ip) {
		ipCopy := make(net.IP, len(ip))
		copy(ipCopy, ip)
		ips = append(ips, ipCopy)
	}
	if len(ips) <= 2 {
		return ips, nil
	}
	return ips[1 : len(ips)-1], nil // drop network and broadcast
}

func incrementIP(ip net.IP) net.IP {
	out := make(net.IP, len(ip))
	copy(out, ip)
	for j := len(out) - 1; j >= 0; j-- {
		out[j]++
		if out[j] != 0 {
			break
		}
	}
	return out
}
