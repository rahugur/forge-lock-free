# Research Assistant Agent

Multi-step research using the Plan-and-Execute workflow.

## How it works

1. **Plan phase**: The LLM creates a research plan:
   - Define key questions to answer
   - Identify information sources to query
   - Plan cross-referencing steps

2. **Execute phase**: Each research step runs as a ReAct sub-session:
   - `web_search` — find relevant articles, papers, documentation
   - `file_read` — read local documents, data files
   - `calculator` — compute statistics, comparisons

3. **Synthesize phase**: The LLM compiles findings into a structured report.

## Usage

```bash
# CLI mode
./forge -w plan-execute \
  -p "Compare the performance characteristics of Redis vs Memcached for session storage at 10K+ concurrent connections" \
  -c examples/research_assistant/config.json

# Server mode
./forge --serve -c examples/research_assistant/config.json
curl -X POST http://localhost:8080/api/sessions \
  -H "Content-Type: application/json" \
  -d '{
    "prompt": "What are the trade-offs between gRPC and REST for internal microservice communication?",
    "workflow": "plan-execute"
  }'
```

## Example output

```
Research Report: gRPC vs REST for Internal Microservices

1. Performance:
   - gRPC: ~10x lower latency (binary protobuf, HTTP/2 multiplexing)
   - REST: Higher overhead (JSON serialization, HTTP/1.1)

2. Developer Experience:
   - gRPC: Strong typing via .proto files, code generation
   - REST: More flexible, easier debugging (curl, browser)

3. Ecosystem:
   - gRPC: Better for polyglot services, streaming support
   - REST: Broader tooling, API gateways, documentation (OpenAPI)

Recommendation: gRPC for performance-critical internal services,
REST for external APIs and services with browser clients.
```
