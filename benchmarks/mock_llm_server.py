#!/usr/bin/env python3
"""
Mock LLM server with configurable latency.
Used by both Forge and LangChain benchmarks to ensure identical backend conditions.

Usage:
    python3 mock_llm_server.py [--port PORT] [--latency-ms MS]
"""

import argparse
import json
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

# Global config
LATENCY_MS = 0
call_count = 0
call_count_lock = threading.Lock()


class MockLLMHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # suppress request logs

    def do_POST(self):
        global call_count

        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8")

        with call_count_lock:
            call_count += 1

        # Simulate LLM latency.
        if LATENCY_MS > 0:
            time.sleep(LATENCY_MS / 1000.0)

        # Check if this is a follow-up (has tool results).
        has_tool_result = '"role": "tool"' in body or '"role":"tool"' in body

        if has_tool_result:
            # Second call: return final answer.
            response = {
                "choices": [
                    {
                        "message": {
                            "role": "assistant",
                            "content": "The answer is 42.",
                        }
                    }
                ],
                "usage": {"prompt_tokens": 20, "completion_tokens": 5},
            }
        else:
            # First call: return a tool call.
            response = {
                "choices": [
                    {
                        "message": {
                            "role": "assistant",
                            "content": None,
                            "tool_calls": [
                                {
                                    "id": "call_1",
                                    "type": "function",
                                    "function": {
                                        "name": "calculator",
                                        "arguments": '{"expression": "6*7"}',
                                    },
                                }
                            ],
                        }
                    }
                ],
                "usage": {"prompt_tokens": 10, "completion_tokens": 5},
            }

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        resp_bytes = json.dumps(response).encode("utf-8")
        self.send_header("Content-Length", str(len(resp_bytes)))
        self.end_headers()
        self.wfile.write(resp_bytes)


def run_server(port: int, latency_ms: int):
    global LATENCY_MS
    LATENCY_MS = latency_ms
    server = HTTPServer(("127.0.0.1", port), MockLLMHandler)
    print(f"Mock LLM server on port {port} (latency={latency_ms}ms)")
    server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Mock LLM server")
    parser.add_argument("--port", type=int, default=9999)
    parser.add_argument("--latency-ms", type=int, default=0)
    args = parser.parse_args()
    run_server(args.port, args.latency_ms)
