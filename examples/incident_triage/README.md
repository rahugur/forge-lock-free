# Incident Triage Agent

Automated incident triage using the Plan-and-Execute workflow.

## How it works

1. **Plan phase**: The LLM generates a triage plan:
   - Classify severity based on symptoms
   - Identify affected services
   - Check recent changes/deployments
   - Look up relevant runbook entries

2. **Execute phase**: Each step runs as a ReAct sub-session with tool access:
   - `shell` — query monitoring systems, check service status
   - `web_search` — look up error messages, known issues
   - `file_read` — read runbooks, configs, deployment manifests

3. **Synthesize phase**: The LLM produces a structured triage report.

## Usage

```bash
# CLI mode
./forge -w plan-execute -p "High error rate on payment-service, 5xx responses spiking to 15%" \
  -c examples/incident_triage/config.json

# Server mode
./forge --serve -c examples/incident_triage/config.json
curl -X POST http://localhost:8080/api/sessions \
  -H "Content-Type: application/json" \
  -d '{
    "prompt": "Alert: payment-service latency p99 > 5s, error rate 12%",
    "workflow": "plan-execute"
  }'
```

## Example output

```
Triage Report:
- Severity: P1 (service degradation, revenue impact)
- Affected: payment-service, checkout-service (downstream)
- Root cause: Likely related to deployment d-12345 (30 min ago, DB schema change)
- Immediate actions:
  1. Rollback deployment d-12345
  2. Scale payment-service to 3x replicas
  3. Page on-call DBA for schema review
```
