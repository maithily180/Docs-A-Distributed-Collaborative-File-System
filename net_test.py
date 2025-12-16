#!/usr/bin/env python3
"""
Docs++ network helper utilities.

Provides two primary commands:
  * ping      - quick latency/connectivity check to NM/SS endpoints.
  * roundtrip - boots local NM/SS/client binaries and performs a CREATE/WRITE/READ cycle.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional, Sequence, Tuple


def _tcp_ping(host: str, port: int, timeout: float) -> Tuple[bool, Optional[float], Optional[str]]:
    """Return (success, latency_ms, error_message)."""
    start = time.perf_counter()
    try:
        with socket.create_connection((host, port), timeout=timeout):
            latency = (time.perf_counter() - start) * 1000.0
            return True, latency, None
    except OSError as exc:
        return False, None, str(exc)


def _wait_for_port(host: str, port: int, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        success, _, _ = _tcp_ping(host, port, timeout=1.0)
        if success:
            return True
        time.sleep(0.2)
    return False


def _repo_root() -> Path:
    return Path(__file__).resolve().parent


def _resolve_bin(name: str) -> str:
    candidates = [
        _repo_root() / "bin" / name,
        _repo_root() / "bin" / f"{name}.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError(f"Could not find compiled binary for '{name}' in bin/")


def _start_process(cmd: Sequence[str], log_name: str) -> subprocess.Popen:
    logs_dir = _repo_root() / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    log_path = logs_dir / log_name
    with open(log_path, "w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT)
    print(f"[net-test] spawned {' '.join(cmd)} (logs -> {log_path})")
    return proc


def _stop_process(proc: Optional[subprocess.Popen]) -> None:
    if not proc:
        return
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def cmd_ping(args: argparse.Namespace) -> int:
    print(f"[ping] NM {args.nm_ip}:{args.nm_port}")
    ok_nm, lat_nm, err_nm = _tcp_ping(args.nm_ip, args.nm_port, args.timeout)
    if ok_nm:
        print(f"  -> reachable in {lat_nm:.2f} ms")
    else:
        print(f"  -> unreachable ({err_nm})")
    if args.ss_ip and args.ss_port:
        print(f"[ping] SS {args.ss_ip}:{args.ss_port}")
        ok_ss, lat_ss, err_ss = _tcp_ping(args.ss_ip, args.ss_port, args.timeout)
        if ok_ss:
            print(f"  -> reachable in {lat_ss:.2f} ms")
        else:
            print(f"  -> unreachable ({err_ss})")
    return 0 if ok_nm else 1


def _build_client_payload(filename: str) -> str:
    commands = [
        f"CREATE {filename}",
        f"WRITE {filename} 0",
        "0 Hello from net_test.",
        "ETIRW",
        f"READ {filename}",
        "QUIT",
    ]
    return "\n".join(commands) + "\n"


def cmd_roundtrip(args: argparse.Namespace) -> int:
    try:
        nm_bin = _resolve_bin("nm")
        ss_bin = _resolve_bin("ss")
        client_bin = _resolve_bin("client")
    except FileNotFoundError as exc:
        print(f"[roundtrip] {exc}", file=sys.stderr)
        print("Please run `make all` first.", file=sys.stderr)
        return 1

    filename = args.file_name or f"net_test_{int(time.time())}.txt"
    nm_cmd = [
        nm_bin,
        "--host",
        args.nm_host,
        "--port",
        str(args.nm_client_port),
        "--ss-port",
        str(args.nm_ss_port),
    ]
    if args.component_verbose:
        nm_cmd.append("--verbose")
    if args.exec_allow:
        nm_cmd.append("--exec-allow")

    ss_cmd = [
        ss_bin,
        "--host",
        args.ss_host,
        "--client-port",
        str(args.ss_client_port),
        "--admin-port",
        str(args.ss_admin_port),
        "--nm-ip",
        args.nm_ip,
        "--nm-port",
        str(args.nm_ss_port),
        "--ss-id",
        args.ss_id,
    ]
    if args.ss_advertise_ip:
        ss_cmd.extend(["--advertise-ip", args.ss_advertise_ip])
    if args.component_verbose:
        ss_cmd.append("--verbose")

    nm_proc = ss_proc = None
    try:
        nm_proc = _start_process(nm_cmd, "nettest-nm.log")
        if not _wait_for_port(args.nm_ip, args.nm_client_port, args.wait_timeout):
            print("[roundtrip] NM port did not open in time", file=sys.stderr)
            return 1

        ss_proc = _start_process(ss_cmd, "nettest-ss.log")
        if not _wait_for_port(args.ss_ip, args.ss_client_port, args.wait_timeout):
            print("[roundtrip] SS client port did not open in time", file=sys.stderr)
            return 1

        # Give the SS time to register with NM before running the client.
        time.sleep(1.0)

        client_cmd = [
            client_bin,
            "--nm-ip",
            args.nm_ip,
            "--nm-port",
            str(args.nm_client_port),
            "--username",
            args.username,
        ]
        if args.component_verbose:
            client_cmd.append("--verbose")

        payload = _build_client_payload(filename)
        print(f"[roundtrip] launching client scenario for {filename}")
        client_proc = subprocess.Popen(
            client_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            stdout, stderr = client_proc.communicate(payload, timeout=args.client_timeout)
        except subprocess.TimeoutExpired:
            client_proc.kill()
            stdout, stderr = client_proc.communicate()
            print("[roundtrip] client timed out", file=sys.stderr)
            print(stdout)
            print(stderr, file=sys.stderr)
            return 1

        success = "Hello from net_test." in stdout and client_proc.returncode == 0
        print("[roundtrip] client output:\n" + stdout)
        if stderr:
            print("[roundtrip] client stderr:\n" + stderr, file=sys.stderr)

        if success:
            print("[roundtrip] success: CREATE/WRITE/READ verified end-to-end.")
            return 0

        print("[roundtrip] failure: could not observe expected READ output.", file=sys.stderr)
        return 1
    finally:
        if not args.keep_procs:
            _stop_process(ss_proc)
            _stop_process(nm_proc)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Docs++ network diagnostics")
    subparsers = parser.add_subparsers(dest="command", required=True)

    ping_parser = subparsers.add_parser("ping", help="Check TCP reachability of NM/SS ports")
    ping_parser.add_argument("--nm-ip", default="127.0.0.1", help="NM IP or hostname to probe")
    ping_parser.add_argument("--nm-port", type=int, default=8000, help="NM client port")
    ping_parser.add_argument("--ss-ip", help="SS IP or hostname to probe")
    ping_parser.add_argument("--ss-port", type=int, help="SS client port")
    ping_parser.add_argument("--timeout", type=float, default=2.0, help="Connection timeout in seconds")
    ping_parser.set_defaults(func=cmd_ping)

    roundtrip_parser = subparsers.add_parser(
        "roundtrip",
        help="Start local NM/SS/client binaries and run a CREATE/WRITE/READ smoke test",
    )
    roundtrip_parser.add_argument("--nm-host", default="0.0.0.0", help="Bind address for NM server sockets")
    roundtrip_parser.add_argument("--nm-ip", default="127.0.0.1", help="IP the SS/client use to reach the NM")
    roundtrip_parser.add_argument("--nm-client-port", type=int, default=8000, help="NM client port")
    roundtrip_parser.add_argument("--nm-ss-port", type=int, default=8001, help="NM storage registration port")
    roundtrip_parser.add_argument("--ss-host", default="0.0.0.0", help="Bind address for SS client/admin sockets")
    roundtrip_parser.add_argument("--ss-ip", default="127.0.0.1", help="IP used to probe SS readiness")
    roundtrip_parser.add_argument("--ss-client-port", type=int, default=9000, help="SS client-facing port")
    roundtrip_parser.add_argument("--ss-admin-port", type=int, default=9100, help="SS admin port")
    roundtrip_parser.add_argument("--ss-id", default="nettest-ss", help="Identifier passed to SS during registration")
    roundtrip_parser.add_argument("--ss-advertise-ip", help="Override SS advertise IP (defaults to detected interface)")
    roundtrip_parser.add_argument("--username", default="nettest-user", help="Username for the scripted client run")
    roundtrip_parser.add_argument("--file-name", help="Test filename (defaults to timestamped net_test_<n>.txt)")
    roundtrip_parser.add_argument("--wait-timeout", type=float, default=15.0, help="Seconds to wait for server sockets")
    roundtrip_parser.add_argument("--client-timeout", type=float, default=30.0, help="Seconds to wait for client completion")
    roundtrip_parser.add_argument(
        "--component-verbose",
        action="store_true",
        help="Pass --verbose to NM/SS/Client for detailed socket logs",
    )
    roundtrip_parser.add_argument(
        "--exec-allow",
        action="store_true",
        help="Start NM with --exec-allow (defaults to restricted EXEC)",
    )
    roundtrip_parser.add_argument(
        "--keep-procs",
        action="store_true",
        help="Do not terminate NM/SS when the test finishes (manual debugging)",
    )
    roundtrip_parser.set_defaults(func=cmd_roundtrip)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("\n[net-test] interrupted", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

