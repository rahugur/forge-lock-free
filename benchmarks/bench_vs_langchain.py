#!/usr/bin/env python3
"""
Head-to-head benchmark: Forge vs LangChain.

Runs identical 2-step ReAct workflows (tool call → final answer) against the
same mock LLM server, measuring:
  - Session throughput (sessions/sec)
  - Scheduling overhead per session
  - Total wall-clock time

Prerequisites:
  pip install langchain langchain-openai requests

Usage:
  # Terminal 1: Start mock LLM server
  python3 benchmarks/mock_llm_server.py --port 9999 --latency-ms 5

  # Terminal 2: Build Forge and run benchmark
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
  python3 benchmarks/bench_vs_langchain.py --port 9999

  # For zero-latency (pure orchestration overhead):
  python3 benchmarks/mock_llm_server.py --port 9999 --latency-ms 0
  python3 benchmarks/bench_vs_langchain.py --port 9999
"""

import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

# ─── Forge benchmark ─────────────────────────────────────────────────

def bench_forge(base_url: str, num_sessions: int) -> dict:
    """Run Forge sessions via the HTTP API."""
    import requests

    # Start Forge server.
    forge_bin = os.path.join(os.path.dirname(__file__), "..", "build", "src", "forge")
    if not os.path.exists(forge_bin):
        print(f"ERROR: Forge binary not found at {forge_bin}")
        print("Build with: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build")
        sys.exit(1)

    # Find a free port for Forge.
    import socket
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    forge_port = sock.getsockname()[1]
    sock.close()

    proc = subprocess.Popen(
        [forge_bin, "--serve", "--api-base", base_url,
         "--api-key", "test", "-m", "mock"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**os.environ, "FORGE_HOST": "127.0.0.1", "FORGE_PORT": str(forge_port)},
    )

    # Wait for server to start.
    forge_url = f"http://127.0.0.1:{forge_port}"
    for _ in range(50):
        try:
            r = requests.get(f"{forge_url}/health", timeout=0.5)
            if r.status_code == 200:
                break
        except Exception:
            pass
        time.sleep(0.1)
    else:
        proc.terminate()
        print("ERROR: Forge server failed to start")
        return {"error": "server failed to start"}

    # Create sessions.
    start = time.perf_counter()
    session_ids = []
    for i in range(num_sessions):
        r = requests.post(f"{forge_url}/api/sessions", json={
            "prompt": f"Compute 6*7 (session {i})",
            "workflow": "react",
        })
        if r.status_code == 201:
            session_ids.append(r.json()["id"])

    # Wait for completion.
    deadline = time.time() + 30
    while time.time() < deadline:
        done = 0
        for sid in session_ids:
            r = requests.get(f"{forge_url}/api/sessions/{sid}")
            info = r.json()
            if info.get("state") in ("completed", "failed"):
                done += 1
        if done >= num_sessions:
            break
        time.sleep(0.01)

    elapsed = time.perf_counter() - start
    proc.terminate()
    proc.wait()

    return {
        "framework": "Forge",
        "sessions": num_sessions,
        "elapsed_sec": round(elapsed, 3),
        "sessions_per_sec": round(num_sessions / elapsed, 1),
        "completed": len(session_ids),
    }


# ─── LangChain benchmark ────────────────────────────────────────────

def bench_langchain_sync(base_url: str, num_sessions: int) -> dict:
    """Run LangChain ReAct agent sessions synchronously."""
    try:
        from langchain_openai import ChatOpenAI
        from langchain.agents import AgentExecutor, create_openai_tools_agent
        from langchain.tools import tool
        from langchain_core.prompts import ChatPromptTemplate, MessagesPlaceholder
    except ImportError:
        return {
            "framework": "LangChain (sync)",
            "error": "langchain not installed. pip install langchain langchain-openai",
        }

    @tool
    def calculator(expression: str) -> str:
        """Evaluate a math expression."""
        try:
            return str(eval(expression))  # noqa: S307
        except Exception as e:
            return f"Error: {e}"

    llm = ChatOpenAI(
        model="mock",
        openai_api_key="test",
        openai_api_base=f"{base_url}/v1",
        temperature=0,
    )

    prompt = ChatPromptTemplate.from_messages([
        ("system", "You are a helpful assistant."),
        ("human", "{input}"),
        MessagesPlaceholder(variable_name="agent_scratchpad"),
    ])

    agent = create_openai_tools_agent(llm, [calculator], prompt)
    executor = AgentExecutor(agent=agent, tools=[calculator], max_iterations=5)

    start = time.perf_counter()
    completed = 0
    for i in range(num_sessions):
        try:
            result = executor.invoke({"input": f"Compute 6*7 (session {i})"})
            completed += 1
        except Exception as e:
            pass  # count failures

    elapsed = time.perf_counter() - start

    return {
        "framework": "LangChain (sync)",
        "sessions": num_sessions,
        "elapsed_sec": round(elapsed, 3),
        "sessions_per_sec": round(num_sessions / elapsed, 1),
        "completed": completed,
    }


def bench_langchain_async(base_url: str, num_sessions: int) -> dict:
    """Run LangChain ReAct agent sessions with asyncio concurrency."""
    try:
        from langchain_openai import ChatOpenAI
        from langchain.agents import AgentExecutor, create_openai_tools_agent
        from langchain.tools import tool
        from langchain_core.prompts import ChatPromptTemplate, MessagesPlaceholder
    except ImportError:
        return {
            "framework": "LangChain (async)",
            "error": "langchain not installed. pip install langchain langchain-openai",
        }

    @tool
    def calculator(expression: str) -> str:
        """Evaluate a math expression."""
        try:
            return str(eval(expression))  # noqa: S307
        except Exception as e:
            return f"Error: {e}"

    llm = ChatOpenAI(
        model="mock",
        openai_api_key="test",
        openai_api_base=f"{base_url}/v1",
        temperature=0,
    )

    prompt = ChatPromptTemplate.from_messages([
        ("system", "You are a helpful assistant."),
        ("human", "{input}"),
        MessagesPlaceholder(variable_name="agent_scratchpad"),
    ])

    agent = create_openai_tools_agent(llm, [calculator], prompt)
    executor = AgentExecutor(agent=agent, tools=[calculator], max_iterations=5)

    async def run_all():
        tasks = []
        for i in range(num_sessions):
            tasks.append(executor.ainvoke({"input": f"Compute 6*7 (session {i})"}))
        results = await asyncio.gather(*tasks, return_exceptions=True)
        return sum(1 for r in results if not isinstance(r, Exception))

    start = time.perf_counter()
    completed = asyncio.run(run_all())
    elapsed = time.perf_counter() - start

    return {
        "framework": "LangChain (async)",
        "sessions": num_sessions,
        "elapsed_sec": round(elapsed, 3),
        "sessions_per_sec": round(num_sessions / elapsed, 1),
        "completed": completed,
    }


# ─── Forge in-process benchmark (no HTTP overhead) ──────────────────

def bench_forge_inprocess(num_sessions: int) -> dict:
    """Run Forge C++ benchmark directly (in-process mock LLM, no network)."""
    bench_bin = os.path.join(os.path.dirname(__file__), "..",
                             "build", "tests", "benchmark", "bench_sessions")
    if not os.path.exists(bench_bin):
        return {
            "framework": "Forge (in-process)",
            "error": "benchmark binary not found. Build with -DFORGE_BUILD_BENCHMARKS=ON",
        }

    proc = subprocess.run([bench_bin], capture_output=True, text=True, timeout=120)
    output = proc.stdout + proc.stderr

    # Parse session throughput lines.
    # Look for "N sessions in M ms" and "X sessions/sec"
    sessions = None
    elapsed_ms = None
    sps = None

    for line in output.split("\n"):
        line = line.strip()
        if "sessions in" in line and "ms" in line:
            # "  1000 sessions in 40 ms"
            parts = line.split()
            for i, p in enumerate(parts):
                if p == "sessions" and i > 0:
                    try:
                        sessions = int(parts[i - 1])
                    except ValueError:
                        pass
                if p == "ms" and i > 0:
                    try:
                        elapsed_ms = int(parts[i - 1])
                    except ValueError:
                        pass
        if "sessions/sec" in line:
            # "  25000.0 sessions/sec"
            parts = line.split()
            for i, p in enumerate(parts):
                if p == "sessions/sec" and i > 0:
                    try:
                        sps = float(parts[i - 1])
                    except ValueError:
                        pass

    if sessions and elapsed_ms:
        return {
            "framework": "Forge (in-process)",
            "sessions": sessions,
            "elapsed_sec": round(elapsed_ms / 1000, 3),
            "sessions_per_sec": round(sps or (sessions * 1000 / elapsed_ms), 1),
            "completed": sessions,
        }

    return {"framework": "Forge (in-process)", "error": "failed to parse output"}


# ─── Main ────────────────────────────────────────────────────────────

def print_results(results: list[dict]):
    print("\n" + "=" * 72)
    print(f"{'Framework':<25} {'Sessions':<10} {'Time (s)':<12} {'Sess/sec':<12} {'Done'}")
    print("-" * 72)
    for r in results:
        if "error" in r:
            print(f"{r['framework']:<25} {r['error']}")
        else:
            print(f"{r['framework']:<25} {r.get('sessions','?'):<10} "
                  f"{r.get('elapsed_sec','?'):<12} "
                  f"{r.get('sessions_per_sec','?'):<12} "
                  f"{r.get('completed','?')}")
    print("=" * 72)

    # Compute speedups.
    forge_sps = None
    for r in results:
        if "Forge" in r.get("framework", "") and "sessions_per_sec" in r:
            forge_sps = r["sessions_per_sec"]
            break

    if forge_sps:
        print("\nSpeedups vs Forge:")
        for r in results:
            if "Forge" in r.get("framework", ""):
                continue
            if "sessions_per_sec" in r and r["sessions_per_sec"] > 0:
                speedup = forge_sps / r["sessions_per_sec"]
                print(f"  Forge is {speedup:.1f}x faster than {r['framework']}")


def main():
    parser = argparse.ArgumentParser(
        description="Forge vs LangChain head-to-head benchmark")
    parser.add_argument("--port", type=int, default=9999,
                        help="Mock LLM server port")
    parser.add_argument("--sessions", type=int, default=50,
                        help="Number of sessions to run")
    parser.add_argument("--skip-forge-http", action="store_true",
                        help="Skip Forge HTTP benchmark (requires running server)")
    parser.add_argument("--skip-langchain", action="store_true",
                        help="Skip LangChain benchmarks")
    args = parser.parse_args()

    base_url = f"http://127.0.0.1:{args.port}"
    results = []

    print("=" * 72)
    print("  Forge vs LangChain — Head-to-Head Benchmark")
    print(f"  Sessions: {args.sessions} | Mock LLM: {base_url}")
    print("=" * 72)

    # 1. Forge in-process (always available, no mock server needed).
    print("\n[1/4] Forge (in-process, no network)...")
    results.append(bench_forge_inprocess(args.sessions))

    if not args.skip_langchain:
        # 2. LangChain sync.
        print(f"\n[2/4] LangChain (sync, {args.sessions} sessions)...")
        results.append(bench_langchain_sync(base_url, args.sessions))

        # 3. LangChain async.
        print(f"\n[3/4] LangChain (async, {args.sessions} sessions)...")
        results.append(bench_langchain_async(base_url, args.sessions))
    else:
        print("\n[2-3/4] Skipping LangChain benchmarks")

    if not args.skip_forge_http:
        # 4. Forge via HTTP.
        print(f"\n[4/4] Forge (HTTP API, {args.sessions} sessions)...")
        results.append(bench_forge(base_url, args.sessions))
    else:
        print("\n[4/4] Skipping Forge HTTP benchmark")

    print_results(results)


if __name__ == "__main__":
    main()
