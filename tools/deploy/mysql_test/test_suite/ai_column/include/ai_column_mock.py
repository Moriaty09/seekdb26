#!/usr/bin/env python3
"""Deterministic OpenAI-compatible HTTP mock for AI column mysqltests."""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


def state_dir(port):
    return "/tmp/seekdb_ai_column_mock_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def read_events(port):
    events = []
    path = state_path(port, "events.jsonl")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    return events


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
    except (ValueError, ProcessLookupError):
        pass
    try:
        os.unlink(pid_path)
    except FileNotFoundError:
        pass


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    for name in ("events.jsonl", "server.out"):
        with open(state_path(port, name), "w", encoding="utf-8"):
            pass
    output = open(state_path(port, "server.out"), "a", encoding="utf-8")
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(process.pid))
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.02)
    raise RuntimeError("AI column mock did not start on port %d" % port)


def serve(port):
    lock = threading.Lock()
    attempts = {}
    log_path = state_path(port, "events.jsonl")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            messages = body.get("messages", [])
            system_text = ""
            user_text = ""
            for message in messages:
                if message.get("role") == "system":
                    system_text = str(message.get("content", ""))
                elif message.get("role") == "user":
                    user_text = str(message.get("content", ""))
            if parsed.path.endswith("/embeddings") or parsed.path.endswith("/api/embed"):
                input_value = body.get("input", "")
                if isinstance(input_value, list):
                    user_text = "|".join(str(value) for value in input_value)
                else:
                    user_text = str(input_value)
            elif parsed.path.endswith("/rerank"):
                user_text = "|".join(str(value) for value in body.get("documents", []))
            key = parsed.path + "|" + user_text
            with lock:
                attempts[key] = attempts.get(key, 0) + 1
                attempt = attempts[key]

            if "slow-old" in user_text:
                time.sleep(1.2)
            else:
                time.sleep(0.05)

            fail = "always-fail" in parsed.path or (
                "retry-once" in user_text and attempt == 1
            )
            if fail:
                status = 400
                response = {"error": {"message": "injected failure"}}
            elif parsed.path.endswith("/embeddings") or parsed.path.endswith("/api/embed"):
                values = body.get("input", [])
                if not isinstance(values, list):
                    values = [values]

                requested_dimension = body.get(
                    "dimensions", body.get("dimension", 4)
                )
                if not isinstance(requested_dimension, int) or requested_dimension <= 0:
                    requested_dimension = 4

                def vector(value, index):
                    text = str(value)
                    if "vec-zero" in text:
                        return [0.0] * requested_dimension
                    if "vec-ten" in text:
                        return [10.0] + [0.0] * (requested_dimension - 1)
                    if "vec-twenty" in text:
                        return [20.0] + [0.0] * (requested_dimension - 1)
                    seed = [float(index + 1), 0.25, 0.5, 0.75]
                    return (seed + [0.0] * requested_dimension)[:requested_dimension]

                status = 200
                embeddings = [vector(value, index) for index, value in enumerate(values)]
                if parsed.path.endswith("/api/embed"):
                    response = {"embeddings": embeddings}
                else:
                    data = [
                            {"index": index, "embedding": embedding}
                            for index, embedding in enumerate(embeddings)
                        ]
                    if any("reverse-order" in str(value) for value in values):
                        data.reverse()
                    response = {"data": data}
            elif parsed.path.endswith("/api/generate"):
                status = 200
                response = {"response": "mock:" + str(body.get("prompt", ""))}
            elif parsed.path.endswith("/rerank"):
                documents = body.get("documents", [])
                status = 200
                response = {
                    "results": sorted(
                        [
                            {
                                "index": index,
                                "relevance_score": float(len(str(document)) % 3),
                            }
                            for index, document in enumerate(documents)
                        ],
                        key=lambda item: (-item["relevance_score"], item["index"]),
                    )
                }
            else:
                if "malformed-json" in user_text and "valid JSON value" in system_text:
                    content = "{invalid JSON"
                elif "base-10 integer" in system_text:
                    content = "7"
                elif "valid JSON value" in system_text:
                    content = json.dumps(
                        {"category": "mock", "input": user_text},
                        ensure_ascii=False,
                    )
                else:
                    content = "mock:" + user_text
                status = 200
                response = {
                    "choices": [{"message": {"role": "assistant", "content": content}}]
                }

            event = {
                "path": parsed.path,
                "status": status,
                "attempt": attempt,
                "authorization": self.headers.get("Authorization", ""),
                "system": system_text,
                "user": user_text,
            }
            with lock:
                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + "\n")
            payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


def assert_events(port, assertion):
    events = read_events(port)
    if assertion == "no-requests":
        ok = not events
    elif assertion == "no-rollback-value":
        ok = not any("rollback-value" in event["user"] for event in events)
    elif assertion == "json-prompt":
        ok = any(
            "valid JSON value" in event["system"]
            and event["authorization"].startswith("Bearer ")
            for event in events
        )
    elif assertion == "typed-prompts":
        has_json = any("valid JSON value" in event["system"] for event in events)
        has_integer = any("base-10 integer" in event["system"] for event in events)
        has_auth = events and all(
            event["authorization"].startswith("Bearer ") for event in events
        )
        ok = has_json and has_integer and has_auth
    elif assertion == "retry-once":
        retry_events = [event for event in events if "retry-once" in event["user"]]
        ok = (
            len(retry_events) == 2
            and [event["status"] for event in retry_events] == [400, 200]
        )
    elif assertion == "stale-guard":
        ok = (
            any("slow-old" in event["user"] for event in events)
            and any("fast-new" in event["user"] for event in events)
        )
    elif assertion == "mixed-once":
        failed = [event for event in events if event["user"] == "malformed-json"]
        recovered = [event for event in events if event["user"] == "lease-recovery"]
        ok = (
            sum("valid JSON value" in event["system"] for event in failed) == 3
            and sum("valid JSON value" not in event["system"] for event in failed) == 1
            and len(recovered) == 2
        )
    elif assertion == "embedding":
        ok = any(event["path"].endswith("/embeddings") for event in events)
    else:
        raise ValueError("unknown assertion: " + assertion)
    if not ok:
        raise AssertionError("assertion %s failed: %r" % (assertion, events))
    print("mock_%s=ok" % assertion.replace("-", "_"))


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: ai_column_mock.py MODE PORT [ASSERTION]")
    mode = sys.argv[1]
    port = int(sys.argv[2])
    if mode == "start":
        start_server(port)
    elif mode == "stop":
        stop_server(port)
    elif mode == "serve":
        serve(port)
    elif mode == "reset":
        with open(state_path(port, "events.jsonl"), "w", encoding="utf-8"):
            pass
    elif mode == "assert":
        assert_events(port, sys.argv[3])
    else:
        raise SystemExit("unknown mode: " + mode)


if __name__ == "__main__":
    main()
