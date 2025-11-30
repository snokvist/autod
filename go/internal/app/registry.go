package app

import (
	"sync"
	"time"
)

// Node represents a discovered or registered process.
type Node struct {
	ID       string    `json:"id"`
	Address  string    `json:"address"`
	Role     Mode      `json:"role"`
	Slots    []string  `json:"slots"`
	LastSeen time.Time `json:"last_seen"`
	Healthy  bool      `json:"healthy"`
	Source   string    `json:"source"`
}

// Registry tracks nodes and slot bindings.
type Registry struct {
	mu    sync.RWMutex
	nodes map[string]*Node
	slots map[string]string
}

// NewRegistry returns an initialized Registry.
func NewRegistry() *Registry {
	return &Registry{
		nodes: make(map[string]*Node),
		slots: make(map[string]string),
	}
}

// UpsertNode records or updates a node entry.
func (r *Registry) UpsertNode(n Node) {
	r.mu.Lock()
	defer r.mu.Unlock()
	existing, ok := r.nodes[n.ID]
	if ok {
		existing.Address = n.Address
		existing.Role = n.Role
		existing.Slots = n.Slots
		existing.LastSeen = n.LastSeen
		existing.Healthy = n.Healthy
		existing.Source = n.Source
		return
	}
	r.nodes[n.ID] = &n
}

// AllNodes returns a copy of the node list.
func (r *Registry) AllNodes() []Node {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]Node, 0, len(r.nodes))
	for _, n := range r.nodes {
		copyNode := *n
		out = append(out, copyNode)
	}
	return out
}

// SetSlotBinding assigns a slot to a node.
func (r *Registry) SetSlotBinding(slot, nodeID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.slots[slot] = nodeID
}

// SlotBinding returns the node bound to a slot.
func (r *Registry) SlotBinding(slot string) (string, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	id, ok := r.slots[slot]
	return id, ok
}

// SlotMap returns a snapshot of slot assignments.
func (r *Registry) SlotMap() map[string]string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make(map[string]string, len(r.slots))
	for k, v := range r.slots {
		out[k] = v
	}
	return out
}

// GetNode returns a node by id.
func (r *Registry) GetNode(id string) (*Node, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	n, ok := r.nodes[id]
	if !ok {
		return nil, false
	}
	copyNode := *n
	return &copyNode, true
}
