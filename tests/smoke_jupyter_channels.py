#!/usr/bin/env python3
"""Smoke test the educational Jupyter-style ZeroMQ channels.

Run while ./build/kernel_worker is active:

    python3 tests/smoke_jupyter_channels.py

This script intentionally depends on jupyter_client so it can verify that the
connection file and basic Jupyter message flow are usable from a normal client.
"""

from __future__ import annotations

import pathlib
import queue
import sys
import time

try:
    from jupyter_client import BlockingKernelClient
except ImportError:
    print("jupyter_client is required: pip install jupyter_client", file=sys.stderr)
    sys.exit(2)


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONNECTION_FILE = ROOT / "data" / "kernel-connection.json"


def wait_for_iopub(client: BlockingKernelClient, msg_type: str, timeout: float = 5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            msg = client.get_iopub_msg(timeout=0.5)
        except queue.Empty:
            continue
        if msg["header"].get("msg_type") == msg_type:
            return msg
    raise TimeoutError(f"timed out waiting for IOPub {msg_type}")


def main() -> int:
    if not CONNECTION_FILE.exists():
        print(f"missing connection file: {CONNECTION_FILE}", file=sys.stderr)
        print("start ./build/kernel_worker first", file=sys.stderr)
        return 1

    client = BlockingKernelClient(connection_file=str(CONNECTION_FILE))
    client.load_connection_file()
    client.start_channels()

    try:
        if not client.is_alive():
            print("heartbeat failed", file=sys.stderr)
            return 1

        kernel_info_id = client.kernel_info()
        kernel_info = client.get_shell_msg(timeout=5)
        assert kernel_info["parent_header"]["msg_id"] == kernel_info_id
        assert kernel_info["content"]["language_info"]["name"] == "c"

        execute_id = client.execute('printf("hello from jupyter_client\\n");', allow_stdin=True)
        stream = wait_for_iopub(client, "stream")
        assert "hello from jupyter_client" in stream["content"]["text"]
        reply = client.get_shell_msg(timeout=5)
        assert reply["parent_header"]["msg_id"] == execute_id
        assert reply["content"]["status"] == "ok"

        input_id = client.execute(
            'char name[64]; nb_input("Name: ", name, sizeof name); printf("hi %s\\n", name);',
            allow_stdin=True,
        )
        stdin_msg = client.get_stdin_msg(timeout=5)
        assert stdin_msg["content"]["prompt"] == "Name: "
        client.input("Ada")
        stream = wait_for_iopub(client, "stream")
        assert "hi Ada" in stream["content"]["text"]
        reply = client.get_shell_msg(timeout=5)
        assert reply["parent_header"]["msg_id"] == input_id

        print("jupyter channel smoke test passed")
        return 0
    finally:
        client.stop_channels()


if __name__ == "__main__":
    raise SystemExit(main())
