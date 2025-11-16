import unittest


def build_slave_request(config_caps: str, node_id: str, ack_generation: int) -> dict:
    caps = []
    if config_caps:
        for part in config_caps.split(','):
            part = part.strip()
            if part:
                caps.append(part)
    payload = {
        "id": node_id,
        "ack_generation": ack_generation,
    }
    if caps:
        payload["caps"] = caps
    return payload


def extract_command_paths(response_payload: dict) -> list[str]:
    generation = response_payload.get("generation", 0)
    if not generation:
        return []
    commands = response_payload.get("commands") or []
    paths: list[str] = []
    for entry in commands:
        if isinstance(entry, dict) and entry.get("path"):
            paths.append(entry["path"])
    return paths


def reassign_slots(current: dict[int, str], moves: list[dict]) -> dict[int, str]:
    planned = dict(current)
    for move in moves:
        slave_id = move.get("slave_id") or move.get("id")
        slot = move.get("slot", 0)
        if not slave_id:
            continue
        planned = {idx: sid for idx, sid in planned.items() if sid != slave_id}
        if slot and slot > 0:
            planned[slot] = slave_id
    return dict(sorted(planned.items()))


def parse_sync_reference(value: str) -> tuple[str, str]:
    if not value:
        raise ValueError("missing sync reference")
    lowered = value.lower()
    if "://" in value and not lowered.startswith("sync://"):
        raise ValueError("unsupported scheme")
    cursor = value[7:] if lowered.startswith("sync://") else value
    slash_index = cursor.find("/")
    if slash_index == -1:
        sync_id = cursor
        path = "/sync/register"
    else:
        sync_id = cursor[:slash_index]
        path = cursor[slash_index:]
    if not sync_id:
        raise ValueError("missing sync id")
    return sync_id, path


def should_release_slot(last_seen_ms: int, retention_s: int, now_ms: int) -> bool:
    if retention_s <= 0:
        return False
    if last_seen_ms <= 0:
        return False
    return now_ms - last_seen_ms > retention_s * 1000


def resolve_replay_targets(assignments: dict[int, str],
                           replay_slots: list[int],
                           replay_ids: list[str]) -> set[int]:
    result: set[int] = set()
    for slot in replay_slots:
        if slot not in assignments:
            raise ValueError(f"slot {slot} unassigned")
        result.add(slot)
    for slave_id in replay_ids:
        match = next((slot for slot, sid in assignments.items()
                      if sid == slave_id), 0)
        if not match:
            raise KeyError(slave_id)
        result.add(match)
    return result


def preferred_slot_for_id(preferences: dict[int, str], node_id: str) -> int:
    for slot in sorted(preferences):
        if preferences[slot] == node_id:
            return slot
    return 0


def enforce_preferred_assignment(assignments: dict[int, str],
                                 preferences: dict[int, str],
                                 node_id: str,
                                 max_slots: int = 10) -> dict[int, str]:
    planned: dict[int, str] = dict(assignments)
    preferred_slot = preferred_slot_for_id(preferences, node_id)
    if preferred_slot <= 0:
        return dict(sorted(planned.items()))
    planned = {slot: sid for slot, sid in planned.items() if sid != node_id}
    displaced = planned.pop(preferred_slot, None)
    planned[preferred_slot] = node_id
    if displaced:
        for slot in range(1, max_slots + 1):
            if slot == preferred_slot:
                continue
            if slot not in planned:
                planned[slot] = displaced
                break
    return dict(sorted(planned.items()))


def delete_assignments(assignments: dict[int, str],
                       delete_ids: list[str]) -> dict[int, str]:
    doomed = set(delete_ids)
    return {slot: sid for slot, sid in assignments.items() if sid not in doomed}


def apply_slot_plan(current_assignments: dict[int, str],
                    record_slots: dict[str, int],
                    plan_overrides: dict[int, str | None],
                    max_slots: int = 10) -> tuple[dict[int, str], dict[str, int]]:
    """Mirror sync_master_apply_slot_assignment_locked planning semantics."""

    planned: dict[int, str | None] = {
        slot: current_assignments.get(slot)
        for slot in range(1, max_slots + 1)
    }
    for slot, sid in plan_overrides.items():
        planned[slot] = sid

    assignments = dict(current_assignments)
    records = dict(record_slots)

    for slot in range(1, max_slots + 1):
        new_id = planned.get(slot)
        current_id = assignments.get(slot)
        current_has = bool(current_id)
        new_has = bool(new_id)

        if current_has and new_has and current_id == new_id:
            records[new_id] = slot
            continue

        if current_has and records.get(current_id) == slot:
            records[current_id] = 0

        if new_has:
            assignments[slot] = new_id  # type: ignore[index]
            records[new_id] = slot      # type: ignore[index]
        else:
            assignments.pop(slot, None)

    return assignments, records


class SyncFlowTest(unittest.TestCase):
    def test_slave_request_splits_caps(self) -> None:
        req = build_slave_request("sync,exec, nodes ", "node-1", 7)
        self.assertEqual(req["id"], "node-1")
        self.assertEqual(req["ack_generation"], 7)
        self.assertEqual(req["caps"], ["sync", "exec", "nodes"])

    def test_extract_command_paths_returns_paths(self) -> None:
        response = {
            "generation": 3,
            "commands": [
                {"path": "/usr/bin/slot1", "args": ["--a"]},
                {"path": "/usr/bin/slot1b"},
            ],
        }
        paths = extract_command_paths(response)
        self.assertEqual(paths, ["/usr/bin/slot1", "/usr/bin/slot1b"])

    def test_extract_command_paths_ignores_missing_generation(self) -> None:
        response = {"generation": 0, "commands": [{"path": "/usr/bin/slot1"}]}
        self.assertEqual(extract_command_paths(response), [])

    def test_reassign_slots_handles_swap(self) -> None:
        current = {1: "alpha", 2: "bravo"}
        moves = [{"slave_id": "alpha", "slot": 2}, {"slave_id": "bravo", "slot": 1}]
        planned = reassign_slots(current, moves)
        self.assertEqual(planned[1], "bravo")
        self.assertEqual(planned[2], "alpha")

    def test_parse_sync_reference_default_path(self) -> None:
        sync_id, path = parse_sync_reference("sync://node-master")
        self.assertEqual(sync_id, "node-master")
        self.assertEqual(path, "/sync/register")

    def test_parse_sync_reference_plain_id(self) -> None:
        sync_id, path = parse_sync_reference("my-master")
        self.assertEqual(sync_id, "my-master")
        self.assertEqual(path, "/sync/register")

    def test_parse_sync_reference_with_custom_path(self) -> None:
        sync_id, path = parse_sync_reference("sync://master-1/custom/register")
        self.assertEqual(sync_id, "master-1")
        self.assertEqual(path, "/custom/register")

    def test_parse_sync_reference_rejects_http(self) -> None:
        with self.assertRaises(ValueError):
            parse_sync_reference("http://example.com")

    def test_should_release_slot_respects_disabled_retention(self) -> None:
        now_ms = 50000
        self.assertFalse(should_release_slot(now_ms - 10000, 0, now_ms))
        self.assertFalse(should_release_slot(0, 60, now_ms))

    def test_should_release_slot_after_window(self) -> None:
        now_ms = 100000
        self.assertFalse(should_release_slot(now_ms - 2000, 5, now_ms))
        self.assertTrue(should_release_slot(now_ms - 10000, 5, now_ms))

    def test_resolve_replay_targets_combines_sources(self) -> None:
        assignments = {1: "alpha", 2: "bravo"}
        targets = resolve_replay_targets(assignments, [2], ["alpha"])
        self.assertEqual(targets, {1, 2})

    def test_resolve_replay_targets_rejects_empty_slot(self) -> None:
        assignments = {1: "alpha"}
        with self.assertRaises(ValueError):
            resolve_replay_targets(assignments, [3], [])

    def test_resolve_replay_targets_rejects_missing_id(self) -> None:
        assignments = {1: "alpha"}
        with self.assertRaises(KeyError):
            resolve_replay_targets(assignments, [], ["ghost"])

    def test_preferred_slot_for_id_matches_mapping(self) -> None:
        preferences = {1: "alpha", 3: "bravo"}
        self.assertEqual(preferred_slot_for_id(preferences, "alpha"), 1)
        self.assertEqual(preferred_slot_for_id(preferences, "bravo"), 3)
        self.assertEqual(preferred_slot_for_id(preferences, "ghost"), 0)

    def test_apply_slot_plan_preserves_manual_moves(self) -> None:
        assignments = {1: "alpha"}
        records = {"alpha": 1}
        overrides = {1: None, 4: "alpha"}
        new_assignments, new_records = apply_slot_plan(assignments, records, overrides)
        self.assertNotIn(1, new_assignments)
        self.assertEqual(new_assignments[4], "alpha")
        self.assertEqual(new_records["alpha"], 4)

    def test_enforce_preferred_assignment_displaces_placeholder(self) -> None:
        assignments = {1: "bravo", 2: "charlie"}
        preferences = {1: "alpha"}
        planned = enforce_preferred_assignment(assignments, preferences, "alpha")
        self.assertEqual(planned[1], "alpha")
        self.assertEqual(planned[2], "charlie")
        self.assertEqual(planned[3], "bravo")

    def test_enforce_preferred_assignment_waits_when_full(self) -> None:
        assignments = {1: "bravo"}
        preferences = {1: "alpha"}
        planned = enforce_preferred_assignment(assignments, preferences, "alpha",
                                               max_slots=1)
        self.assertEqual(planned, {1: "alpha"})

    def test_delete_assignments_removes_ids(self) -> None:
        assignments = {1: "alpha", 2: "bravo", 3: "charlie"}
        remaining = delete_assignments(assignments, ["bravo"])
        self.assertEqual(remaining, {1: "alpha", 3: "charlie"})

    def test_delete_assignments_ignores_unknown_ids(self) -> None:
        assignments = {1: "alpha"}
        remaining = delete_assignments(assignments, ["ghost"])
        self.assertEqual(remaining, assignments)


if __name__ == "__main__":
    unittest.main()
