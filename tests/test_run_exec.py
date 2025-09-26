import json
import socket
import subprocess
import time
from pathlib import Path
from typing import Optional
from urllib import request


def build_autod(repo_root: Path) -> Path:
    subprocess.run(["make", "native"], cwd=repo_root, check=True)
    return repo_root / "autod"


def get_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(host: str, port: int, timeout: float = 5.0) -> None:
    deadline = time.time() + timeout
    last_err: Optional[Exception] = None
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError as exc:  # pragma: no cover - debug aid
            last_err = exc
            time.sleep(0.05)
    raise RuntimeError(f"Timed out waiting for {host}:{port}") from last_err


def exec_request(port: int, payload: dict) -> dict:
    data = json.dumps(payload).encode("utf-8")
    req = request.Request(
        f"http://127.0.0.1:{port}/exec",
        data=data,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    with request.urlopen(req, timeout=5) as resp:
        body = resp.read().decode("utf-8")
    return json.loads(body)


def test_exec_drains_large_stdout(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    binary = build_autod(repo_root)

    payload_script = tmp_path / "burst_stdout.sh"
    payload_script.write_text("#!/bin/sh\nawk 'BEGIN{for(i=0;i<2048;i++)printf \"A\"}'\n")
    payload_script.chmod(0o755)

    port = get_free_port()
    cfg_path = tmp_path / "autod.conf"
    cfg_path.write_text(
        "[server]\n"
        f"port={port}\n"
        "bind=127.0.0.1\n\n"
        "[exec]\n"
        "interpreter=/bin/sh\n"
        "timeout_ms=2000\n"
        "max_output_bytes=4096\n"
    )

    proc = subprocess.Popen(
        [str(binary), str(cfg_path)],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    stdout_data = b""
    stderr_data = b""
    result = None
    try:
        wait_for_port("127.0.0.1", port)
        result = exec_request(port, {"path": str(payload_script), "args": []})
    finally:
        proc.terminate()
        try:
            stdout_data, stderr_data = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout_data, stderr_data = proc.communicate(timeout=5)

    if proc.returncode not in (0, None):  # pragma: no cover - debugging aid
        raise RuntimeError(
            f"autod exited with {proc.returncode}\nstdout: {stdout_data.decode()}\n"
            f"stderr: {stderr_data.decode()}"
        )

    assert result is not None
    assert result["rc"] == 0
    assert result["stderr"] == ""
    assert result["stdout"] == "A" * 2048
    assert len(result["stdout"]) == 2048
