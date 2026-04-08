# Forge — Lock-Free Agent Orchestration Runtime

> A high-performance C++17 agent runtime that orchestrates LLM-powered workflows using lock-free concurrency primitives. Built to demonstrate that agent orchestration doesn't have to be slow — Forge handles **25,000+ sessions/sec** where Python frameworks like LangChain manage ~50.

---

## Why This Exists

Every major AI agent framework today — LangChain, CrewAI, AutoGen — is written in Python. Python is great for prototyping, but it has a fundamental problem for production agent workloads: **the Global Interpreter Lock (GIL)**. The GIL means only one thread can execute Python bytecode at a time, even on a 64-core server. When you're orchestrating hundreds of concurrent agent sessions, each making LLM calls and executing tools, the framework itself becomes the bottleneck.

Forge asks: **what if the orchestration layer was as fast as the hardware allows?**

This project is a from-scratch C++17 implementation of an agent runtime that uses lock-free data structures (no mutexes on the hot path), work-stealing thread pools, and atomic operations to achieve scheduling overhead measured in **nanoseconds**, not milliseconds.

### What Forge Is

- A **server-side agent runtime** for running hundreds/thousands of concurrent LLM sessions
- A demonstration of **lock-free systems programming** applied to AI orchestration
- A **portfolio project** showcasing C++17 concurrency, systems design, and API design

### What Forge Is Not

- Not a replacement for LangChain's ecosystem (prompt templates, vector stores, document loaders)
- Not a chatbot framework — it's the engine underneath one
- Not production-hardened (no auth, no TLS termination, no k8s manifests — yet)

---

## Results at a Glance

All benchmarks run on Apple Silicon (M-series), single process, with in-process mock LLM (no network).

| Metric | Forge | LangChain | CrewAI | Forge Advantage |
|--------|-------|-----------|--------|-----------------|
| **Scheduling overhead** | ~300 ns/task | ~50-100 us/task | ~100+ us/task | **200-300x faster** |
| **Session throughput** | 25,000 sess/s | ~10-50 sess/s | ~5-20 sess/s | **500-2500x faster** |
| **Memory per session** | <1 KB | ~2-5 MB | ~5-10 MB | **2000-10000x less** |
| **Concurrent scaling** | Linear with cores | GIL-limited | GIL-limited | Truly parallel |

### Why Is The Gap So Large?

1. **No GIL.** Forge threads execute in true parallel. Python's `asyncio` gives concurrency (interleaving) but not parallelism (simultaneous execution).

2. **No object graph.** A LangChain `AgentExecutor` carries chain configs, prompt templates, callback managers, output parsers — all heap-allocated Python objects. A Forge `Session` is a 104-byte struct with atomics and a pointer to its conversation history.

3. **Lock-free scheduling.** Forge's task submission path uses `atomic::exchange` (one CPU instruction). No mutex lock/unlock, no kernel syscall, no memory allocation.

4. **Work-stealing.** When a Forge worker thread blocks waiting for an LLM response, it doesn't sleep — it steals and executes tasks from other workers' queues. Python threads just park.

### Test Results

106 tests passing across all components, verified under three build configurations:

| Build | Tests | What It Checks |
|-------|-------|----------------|
| **Release** | 106/106 pass | Correctness at full optimization |
| **ThreadSanitizer (TSan)** | 106/106 pass | No data races, no lock-order inversions |
| **AddressSanitizer (ASan)** | 106/106 pass | No buffer overflows, no use-after-free, no leaks |

---

## Architecture

```
                              ┌─────────────────┐
                              │   HTTP Client   │
                              │ (curl / browser)│
                              └────────┬────────┘
                                       │ REST + SSE
                              ┌────────▼────────┐
                              │   HTTP Server   │
                              │  (cpp-httplib)  │
                              └────────┬────────┘
                                       │
                              ┌────────▼─────────┐
                              │ SessionManager   │
                              │                  │
                              │ ConcurrentMap    │
                              │ <id, Session*>   │
                              └────────┬─────────┘
                                       │ creates workflow per session
                              ┌────────▼─────────┐
                              │ WorkflowFactory  │
                              │                  │
                              │ "react"          │
                              │ "plan-execute"   │
                              │ "map-reduce"     │
                              └────────┬─────────┘
                                       │ runs on ThreadPool
                     ┌─────────────────┼─────────────────┐
                     │                 │                 │
              ┌──────▼──────┐  ┌───────▼───────┐  ┌──────▼──────┐
              │   ReAct     │  │ Plan-Execute  │  │  Map-Reduce │
              │ (loop)      │  │ (3-phase)     │  │ (parallel)  │
              └──────┬──────┘  └───────┬───────┘  └──────┬──────┘
                     │                 │                  │
              ┌──────▼─────────────────▼──────────────────▼───────┐
              │                                                   │
              │            LLM Client (OpenAI-compatible)         │
              │     ThrottledLLMClient (Semaphore + RateLimiter)  │
              │                                                   │
              ├─────────────────────────────────────────────────  │
              │                                                   │
              │            Tool Executor                          │
              │     (concurrent batch execution via ThreadPool)   │
              │                                                   │
              └──────────────────────┬────────────────────────────┘
                                     │
              ┌──────────────────────▼────────────────────────────┐
              │          Lock-Free Concurrency Layer              │
              │                                                   │
              │  ┌────────────┐  ┌────────────────┐  ┌──────────┐ │
              │  │ MPSC Queue │  │ Work-Stealing  │  │Concurrent│ │
              │  │ (Vyukov)   │  │ Deque (Chase-  │  │ Map      │ │
              │  │            │  │ Lev)           │  │(striped) │ │
              │  └────────────┘  └────────────────┘  └──────────┘ │
              │  ┌────────────┐  ┌────────────────┐  ┌──────────┐ │
              │  │ ThreadPool │  │ Future/Promise │  │Semaphore │ │
              │  │(work-steal)│  │ (lock-free)    │  │(atomic)  │ │
              │  └────────────┘  └────────────────┘  └──────────┘ │
              └───────────────────────────────────────────────────┘
```

### How Data Flows (A Single Request)

Here's what happens when you `POST /api/sessions` with `"prompt": "What is 6*7?"`:

1. **HTTP Server** receives the request, parses JSON, creates a `CreateSessionRequest`
2. **SessionManager** allocates a `Session` (struct with atomics), stores it in `ConcurrentMap`, submits a task to the `ThreadPool`
3. **ThreadPool** assigns the task to a worker via lock-free MPSC queue. Worker picks it up within ~300ns.
4. **WorkflowFactory** creates the requested workflow (e.g., `ReActWorkflow`)
5. **ReAct loop** begins:
   - Calls LLM (via `ThrottledLLMClient` which enforces rate limits and concurrency caps)
   - LLM returns a tool call: `calculator("6*7")`
   - `ToolExecutor` runs the calculator, returns `"42"`
   - Result appended to conversation, LLM called again
   - LLM returns final answer: `"42"`
6. **Session state** transitions: `CREATED → WAITING_FOR_LLM → EXECUTING_TOOLS → WAITING_FOR_LLM → COMPLETED`
7. **SSE subscribers** receive each state change in real-time
8. **GET /api/sessions/:id** returns the final answer

---

## Design Deep Dive (For C++ Beginners)

This section explains the core design decisions. If you're learning C++ concurrency, this project is a good case study because every primitive is implemented from scratch (not just `std::mutex` everywhere).

### Lock-Free MPSC Queue — `src/core/mpsc_queue.h`

**What:** A queue where many threads can push simultaneously (Multi-Producer), but only one thread reads (Single-Consumer).

**Why not just use `std::queue` with a mutex?** Because every `push()` would require: (1) lock the mutex (kernel syscall if contended), (2) push the item, (3) unlock. With 8 threads pushing, they serialize — each waits for the others.

**How Forge does it:** Based on [Dmitry Vyukov's algorithm](http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue). The key insight is `atomic::exchange` — it atomically swaps a pointer and returns the old value, all in one CPU instruction. No thread ever waits for another.

```
Thread A: push(X)          Thread B: push(Y)          Consumer: pop()
  ┌─ exchange head ──┐       ┌─ exchange head ──┐       ┌─ read tail ─┐
  │ prev = head      │       │ prev = head      │       │ follow next │
  │ head = &X        │       │ head = &Y        │       │ return value│
  │ prev->next = &X  │       │ prev->next = &Y  │       └─────────────┘
  └──────────────────┘       └──────────────────┘
  (Both run simultaneously — no waiting!)
```

**Cache-line padding (`alignas(64)`):** The `head_` pointer (written by producers) and `tail_` pointer (read by consumer) are on separate 64-byte cache lines. This prevents "false sharing" — where two CPUs fight over the same cache line even though they're accessing different variables.

### Work-Stealing Deque — `src/core/work_stealing_deque.h`

**What:** A double-ended queue where the owner thread pushes/pops from the bottom (fast, no contention), and other threads can "steal" from the top (requires CAS).

**Why:** This is from the 2005 paper ["Dynamic Circular Work-Stealing Deque" by Chase & Lev](https://dl.acm.org/doi/10.1145/1073970.1073974). It's the same algorithm used in Go's goroutine scheduler, Java's ForkJoinPool, and Rust's Rayon.

**How it works:** Each worker thread has its own deque. When it finishes its tasks, it randomly picks another worker and tries to steal from their deque's top. The steal operation uses `compare_exchange` (CAS) — if two thieves race, only one succeeds; the other retries on a different victim.

```
Worker 0 deque:     Worker 1 deque:     Worker 2 deque:
  [Task A]            [Task D]            (empty)
  [Task B]            [Task E]              ↑
  [Task C]                                  │ steals Task D
    ↑ pop                                   │ from Worker 1
```

### Lock-Free Future/Promise — `src/core/future.h`

**What:** A way to get a result from an asynchronous operation. The producer calls `promise.set_value(x)`, the consumer calls `future.get()` to wait for it.

**How is this different from `std::future`?** `std::future::get()` blocks the thread (it sleeps). Forge's `Future::get()` **does useful work while waiting**:

```cpp
// Inside Future::get() — the key innovation
while (!state_->ready_.load(std::memory_order_acquire)) {
    detail::backoff(iter++);  // This calls yield_fn()!
}
```

The `yield_fn` is set by the ThreadPool. When a worker thread calls `future.get()`, instead of sleeping, it checks: "Is there another task I can execute while I wait?" This prevents **pool starvation** — where all workers are blocked waiting for results, but nobody is processing the tasks that would produce those results.

### Concurrent Map — `src/core/concurrent_map.h`

**What:** A hash map safe for concurrent access. Used by `SessionManager` to store active sessions.

**Why not `std::unordered_map` with a mutex?** A single mutex serializes ALL reads and writes. If 100 threads want to read different sessions, they queue up.

**How Forge does it:** Striped locking with 64 independent buckets, each with its own `shared_mutex`:
- **Readers** (checking session status) take a shared lock — many can read the same bucket simultaneously
- **Writers** (creating/deleting sessions) take an exclusive lock — but only on ONE bucket (1/64th of the keyspace)

This means ~98% of operations never contend with each other.

### Thread Pool — `src/core/thread_pool.h`

**What:** A fixed set of worker threads that execute submitted tasks.

**Design choices:**
- **Two-level task injection:** External threads (HTTP handlers) push to MPSC queues (FIFO — fair ordering). Worker threads spawning subtasks push to their local deque (LIFO — cache-friendly, most recently touched data is hot in L1).
- **Lock-free wake:** Instead of condition variables (`std::condition_variable` requires a mutex!), workers monitor an atomic `epoch_` counter. `notify()` bumps it with `fetch_add`. Workers spin-check, then yield, then sleep with exponential backoff.
- **Work stealing:** Idle workers randomly steal from busy workers' deques.

### Backpressure — Rate Limiter + Semaphore

**Problem:** LLM APIs have rate limits (e.g., 60 requests/sec for OpenAI). If 500 sessions submit LLM calls simultaneously, the API will reject them.

**Solution:** Two layers:
1. **Semaphore** (`src/core/semaphore.h`): Limits concurrent in-flight LLM calls (default: 8). Uses `atomic::fetch_sub` — lock-free acquire, no mutex.
2. **Token Bucket Rate Limiter** (`src/core/rate_limiter.h`): Limits requests/second. Tokens refill at a configured rate. Burst capacity handles spikes.

```cpp
// ThrottledLLMClient wraps any ILLMClient with backpressure
class ThrottledLLMClient : public ILLMClient {
    Future<LLMResponse> complete(messages, tools) override {
        semaphore_.acquire();     // Block if too many in-flight
        rate_limiter_.acquire();  // Block if over rate limit
        auto result = inner_.complete(messages, tools);
        // ... release semaphore when done
    }
};
```

---

## Workflow Patterns

### ReAct (Reasoning + Acting)

The default workflow. Implements the [ReAct pattern](https://arxiv.org/abs/2210.03629):

```
User prompt → LLM thinks → Tool call → Tool result → LLM thinks → Final answer
                 ↑                                         │
                 └─────────────── loop ────────────────────┘
```

Each iteration: call LLM → if it returns tool calls, execute them and loop; if it returns text, that's the final answer. Bounded by `max_iterations` to prevent infinite loops.

### Plan-and-Execute

Three phases for complex multi-step tasks:

```
Phase 1 — PLAN:     LLM generates structured plan
                     {"steps": [{"id": 1, "description": "Research X"}, ...]}

Phase 2 — EXECUTE:  Each step runs as an independent ReAct sub-session
                     Step 1 → ReAct → result
                     Step 2 → ReAct → result
                     Step 3 → ReAct → result

Phase 3 — SYNTHESIZE: LLM combines all step results into final answer
```

Steps execute sequentially. Cancellation is checked between steps. Token counts accumulate across all phases.

### Map-Reduce

For embarrassingly parallel tasks:

```
Phase 1 — DECOMPOSE: LLM splits task into N independent sub-tasks

Phase 2 — MAP:       All sub-tasks run in parallel via ThreadPool
                      ┌─ Sub-task 1 → ReAct → result ─┐
                      ├─ Sub-task 2 → ReAct → result ──┤
                      └─ Sub-task 3 → ReAct → result ──┘

Phase 3 — REDUCE:    LLM aggregates sub-results into final answer
```

Sub-tasks are local `Session` objects (not registered in `SessionManager`). Pool starvation is prevented by `Future::get()`'s inline work-stealing.

### Task DAG

Utility for dependent tool calls (not a workflow, but used within workflows):

```cpp
TaskDAG dag;
auto fetch  = dag.add_node({"web_search", R"({"q":"C++ atomics"})"});
auto parse  = dag.add_node({"extract", R"({"text":"{{node_0_result}}"})"}, {fetch});
auto format = dag.add_node({"format", R"({"data":"{{node_1_result}}"})"}, {parse});

auto results = dag.execute(executor, pool);
// Executes in topological order: fetch → parse → format
// {{node_N_result}} placeholders are substituted with actual outputs
```

---

## Quick Start

### Prerequisites

- C++17 compiler (GCC 8+, Clang 10+, Apple Clang 12+)
- CMake 3.16+
- No external dependencies to install (everything fetched via CMake FetchContent: spdlog, nlohmann_json, cpp-httplib, Catch2, OpenSSL)

### Build

```bash
git clone <repo-url> forge
cd forge

# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)    # Linux
cmake --build build -j$(sysctl -n hw.ncpu)   # macOS

# Run all tests
cd build && ctest --output-on-failure
```

### CLI Mode — Single Prompt

```bash
# Set your API key
export OPENAI_API_KEY="sk-..."

# Basic ReAct (default workflow)
./build/src/forge -p "What is the square root of 144?"

# Plan-Execute workflow
./build/src/forge -p "Research the pros and cons of Rust vs C++" -w plan-execute

# Map-Reduce workflow
./build/src/forge -p "Compare Python, Go, and Rust for web servers" -w map-reduce

# With verbose logging
./build/src/forge -p "What is 6*7?" -v
```

### Server Mode — HTTP API

```bash
# Start the server
./build/src/forge --serve -k $OPENAI_API_KEY

# In another terminal:

# Create a session
curl -s -X POST http://localhost:8080/api/sessions \
  -H "Content-Type: application/json" \
  -d '{"prompt": "What is 2+2?", "workflow": "react"}' | jq .
# → {"id": 1}

# Check session status
curl -s http://localhost:8080/api/sessions/1 | jq .
# → {"id":1, "state":"completed", "step_count":1, "answer":"4", "error":""}

# List all sessions
curl -s http://localhost:8080/api/sessions | jq .

# Cancel a running session
curl -s -X DELETE http://localhost:8080/api/sessions/1 | jq .

# Health check
curl -s http://localhost:8080/health | jq .
# → {"status":"ok", "active_sessions":0}

# Global metrics
curl -s http://localhost:8080/api/metrics | jq .
# → {"sessions_created":1, "sessions_completed":1, "total_prompt_tokens":20, ...}

# Subscribe to live session events (Server-Sent Events)
curl -N http://localhost:8080/api/sessions/1/subscribe
# event: state
# data: {"state":"waiting_for_llm","step_count":1}
#
# event: state
# data: {"state":"executing_tools","step_count":1}
#
# event: state
# data: {"state":"completed","step_count":1,"answer":"42"}
#
# event: done
# data: {}
```

### Using a Free LLM (No API Key Required)

You can test Forge with free LLM providers that expose an OpenAI-compatible API:

#### Option 1: Groq (Recommended — Free Tier, Fast)

1. Sign up at [console.groq.com](https://console.groq.com) — free, no credit card
2. Create an API key
3. Run Forge:

```bash
./build/src/forge \
  --api-base https://api.groq.com/openai \
  -m llama-3.3-70b-versatile \
  -k gsk_YOUR_GROQ_KEY \
  -p "What is the capital of France?"
```

#### Option 2: OpenRouter (Many Free Models)

1. Sign up at [openrouter.ai](https://openrouter.ai) — free tier available
2. Get your API key
3. Run Forge:

```bash
./build/src/forge \
  --api-base https://openrouter.ai/api \
  -m google/gemma-2-9b-it:free \
  -k sk-or-YOUR_KEY \
  -p "Explain recursion in simple terms"
```

#### Option 3: Local with Ollama (No Internet Required)

1. Install [Ollama](https://ollama.ai) and pull a model:
```bash
ollama pull llama3.2
ollama serve  # Starts on port 11434
```

2. Run Forge against it:
```bash
./build/src/forge \
  --api-base http://localhost:11434 \
  -m llama3.2 \
  -k unused \
  -p "What is 6 times 7?"
```

#### Option 4: Mock LLM Server (For Benchmarking)

Test the orchestration without any LLM at all:

```bash
# Terminal 1: Start mock server
python3 benchmarks/mock_llm_server.py --port 9999 --latency-ms 5

# Terminal 2: Run Forge against it
./build/src/forge \
  --api-base http://127.0.0.1:9999 \
  -m mock -k test \
  -p "What is 6*7?"
```

---

## API Reference

### POST /api/sessions

Create a new agent session. Returns immediately; the session runs asynchronously.

**Request Body:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `prompt` | string | *required* | The user prompt |
| `system_message` | string | `"You are a helpful assistant."` | System message for the LLM |
| `workflow` | string | `"react"` | Workflow: `react`, `plan-execute`, `map-reduce` |
| `tools` | string[] | all tools | Tool filter (empty = all registered tools) |
| `max_iterations` | int | 15 | Max ReAct loop iterations per session |
| `timeout_seconds` | int | 300 | Session timeout in seconds |

**Response (201 Created):**

```json
{"id": 1}
```

**Error Responses:**
- `400` — Missing prompt or invalid JSON
- `503` — Max sessions reached or unknown workflow

### GET /api/sessions/:id

Get the current state of a session.

**Response (200):**

```json
{
  "id": 1,
  "state": "completed",
  "step_count": 2,
  "answer": "The answer is 42.",
  "error": ""
}
```

**Session States:** `created` → `waiting_for_llm` → `executing_tools` → (loop) → `completed` | `failed` | `cancelled`

### DELETE /api/sessions/:id

Cooperatively cancel a running session. The workflow checks `cancel_requested` between steps.

### GET /api/sessions

List all sessions with their current state.

### GET /api/sessions/:id/subscribe

**Server-Sent Events** stream for real-time session updates. Emits events on every state change.

| Event | Description |
|-------|-------------|
| `state` | Session state changed. Data includes `state`, `step_count`, `answer`, `error`. |
| `done` | Session reached terminal state. Stream closes. |

```bash
curl -N http://localhost:8080/api/sessions/1/subscribe
```

### GET /api/metrics

Global runtime metrics (atomic counters, no locking overhead).

```json
{
  "sessions_created": 150,
  "sessions_completed": 148,
  "sessions_failed": 2,
  "total_prompt_tokens": 50000,
  "total_completion_tokens": 15000,
  "total_tool_calls": 300,
  "active_sessions": 0
}
```

### GET /health

```json
{"status": "ok", "active_sessions": 5}
```

---

## Configuration

### Config File

```bash
./build/src/forge --serve -c configs/default.json -k $OPENAI_API_KEY
```

**`configs/default.json`:**

```json
{
    "num_threads": 0,
    "max_concurrent_llm_calls": 8,
    "llm_rate_limit": 60.0,
    "llm_rate_burst": 10.0,
    "max_sessions": 1000,
    "default_max_iterations": 15,
    "default_timeout_seconds": 300,
    "log_level": "info",
    "host": "127.0.0.1",
    "port": 8080
}
```

| Field | Description |
|-------|-------------|
| `num_threads` | Worker threads. `0` = auto-detect (hardware concurrency) |
| `max_concurrent_llm_calls` | Max simultaneous LLM requests (semaphore capacity) |
| `llm_rate_limit` | Requests/second to the LLM API |
| `llm_rate_burst` | Token bucket burst capacity |
| `max_sessions` | Max concurrent sessions before returning 503 |
| `default_max_iterations` | Default ReAct loop iteration limit |
| `default_timeout_seconds` | Default session timeout |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `OPENAI_API_KEY` | Default API key (overridden by `-k`) |
| `FORGE_HOST` | Server bind address (overrides config) |
| `FORGE_PORT` | Server port (overrides config) |

---

## Example Agents

Three pre-configured examples demonstrate different workflow patterns:

### Incident Triage (Plan-Execute)

```bash
./build/src/forge -w plan-execute \
  -p "High error rate on payment-service, 500 errors spiking" \
  -c examples/incident_triage/config.json \
  -k $OPENAI_API_KEY
```

The LLM generates a triage plan (classify severity → identify affected services → check dependencies → suggest mitigations), then executes each step.

### Code Review (Map-Reduce)

```bash
./build/src/forge -w map-reduce \
  -p "Review this code for bugs, security issues, and performance" \
  -c examples/code_review/config.json \
  -k $OPENAI_API_KEY
```

The LLM decomposes review into parallel tracks (bugs, security, performance, style), runs them simultaneously, then aggregates findings.

### Research Assistant (Plan-Execute)

```bash
./build/src/forge -w plan-execute \
  -p "Compare React, Vue, and Svelte for a new project" \
  -c examples/research_assistant/config.json \
  -k $OPENAI_API_KEY
```

---

## Benchmarking

### In-Process Benchmark (No Network)

Uses an in-process mock LLM to measure pure orchestration overhead:

```bash
cmake -B build -DFORGE_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_sessions
./build/tests/benchmark/bench_sessions
```

**Sample output:**

```
=== Scheduling Overhead ===
  100000 tasks in 31 ms
  307 ns/task

=== Session Throughput (mock LLM, 0ms latency) ===
  1000 sessions in 40 ms
  25000.0 sessions/sec

=== Memory per Session ===
  RSS delta for 1000 sessions: 800 KB
  ~0.8 KB/session

=== Concurrent Sessions (10ms LLM latency) ===
  100 sessions, 3 steps each, 10ms LLM latency
  Completed in 85 ms
  1176.5 sessions/sec
```

### Head-to-Head vs LangChain

Run the same 2-step ReAct workflow on both Forge and LangChain with an identical mock LLM:

```bash
# Terminal 1: Start mock LLM server
python3 benchmarks/mock_llm_server.py --port 9999 --latency-ms 5

# Terminal 2: Run comparison
pip install langchain langchain-openai
python3 benchmarks/bench_vs_langchain.py --port 9999 --sessions 50
```

This isolates orchestration overhead: both frameworks hit the same backend, so the gap is entirely due to scheduling and runtime efficiency.

---

## Safety Testing

```bash
# ThreadSanitizer — detects data races
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan
cd build-tsan && ctest --output-on-failure

# AddressSanitizer — detects memory errors
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
cd build-asan && ctest --output-on-failure
```

---

## Project Structure

```
src/
  core/           Lock-free primitives
    mpsc_queue.h          Vyukov MPSC queue (multi-producer, single-consumer)
    work_stealing_deque.h Chase-Lev work-stealing deque
    thread_pool.h         Work-stealing thread pool with lock-free wake
    concurrent_map.h      64-stripe concurrent hash map
    future.h              Lock-free Future/Promise with inline work-stealing
    semaphore.h           Atomic counting semaphore
    rate_limiter.h        Token bucket rate limiter
    async_http_client.h   Async HTTP via thread pool

  llm/            LLM integration
    llm_client_interface.h   Abstract ILLMClient interface
    llm_client.h             OpenAI-compatible HTTP client
    throttled_llm_client.h   Backpressure wrapper (semaphore + rate limiter)
    message.h                Message, ToolCall, LLMResponse types

  tools/          Tool system
    tool_registry.h     Tool registration and schema management
    tool_executor.h     Concurrent batch execution via ThreadPool
    builtin/            Built-in tools: calculator, file_read, shell, web_search

  session/        Session management
    session.h               Session state machine (atomic state + conversation)
    session_manager.h       Concurrent session lifecycle (create/query/cancel/GC)
    conversation.h          Thread-safe message history
    summarizing_conversation.h  LLM-based history compression
    cost_tracker.h          Per-session token cost tracking

  workflow/       Agent workflow patterns
    workflow.h          Abstract Workflow base class
    workflow_factory.h  String-name → constructor registry
    react.h/.cpp        ReAct (Thought-Action-Observation loop)
    plan_execute.h/.cpp Plan-and-Execute (3-phase: plan → execute → synthesize)
    map_reduce.h/.cpp   Map-Reduce (decompose → parallel execute → aggregate)
    task_dag.h          Topological DAG executor with placeholder substitution

  server/         HTTP layer
    http_server.h/.cpp  REST API + SSE streaming + metrics endpoint

  utils/          Shared utilities
    config.h        JSON configuration loader
    logging.h       spdlog initialization
    json.h          nlohmann::json type alias
    tracing.h       RAII Span with hierarchical trace/span IDs
    metrics.h       Global atomic counters (sessions, tokens, tool calls)

tests/
  core/           68 tests for lock-free primitives
  tools/          Tool tests
  session/        Session manager, cost tracker, conversation tests
  workflow/       Workflow factory, ReAct, plan-execute, map-reduce, task DAG tests
  stress/         Concurrent session stress tests (100+ sessions under TSan)
  server/         HTTP API + SSE streaming tests
  benchmark/      Throughput and scheduling micro-benchmarks

benchmarks/       Head-to-head Python comparison scripts
examples/         Pre-configured agent examples (incident triage, code review, research)
configs/          Runtime configuration files
```

---

## Forge vs Python Frameworks — When to Use What

| Use Case | Recommendation |
|----------|----------------|
| Prototyping / exploring prompts | LangChain — massive ecosystem, fast iteration |
| Single-agent chatbot | LangChain — battle-tested, lots of examples |
| Multi-agent collaboration | CrewAI — purpose-built for agent teams |
| **100+ concurrent sessions** | **Forge** — linear scaling, no GIL |
| **Latency-sensitive orchestration** | **Forge** — 300ns scheduling vs 100us+ |
| **Embedded / edge deployment** | **Forge** — single binary, <1KB per session |
| **Custom workflow patterns** | **Forge** — WorkflowFactory, TaskDAG |
| **High-throughput batch processing** | **Forge** — 25K sessions/sec |

---

## License

MIT
