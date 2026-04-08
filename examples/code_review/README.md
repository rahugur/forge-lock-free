# Code Review Agent

Automated code review using the Map-Reduce workflow.

## How it works

1. **Map phase**: The LLM decomposes the review into parallel sub-tasks:
   - Bug/logic analysis
   - Security review
   - Performance review
   - Style/maintainability review

2. **Execute phase**: Each sub-task runs as an independent ReAct session, reading files and analyzing code in parallel.

3. **Reduce phase**: The LLM aggregates all findings into a structured review.

## Why Map-Reduce?

Code review sub-tasks are independent — security analysis doesn't depend on style review. Running them in parallel gives ~4x speedup over sequential review.

## Usage

```bash
# CLI mode
./forge -w map-reduce -p "Review the changes in src/session/session_manager.cpp" \
  -c examples/code_review/config.json

# Server mode
./forge --serve -c examples/code_review/config.json
curl -X POST http://localhost:8080/api/sessions \
  -H "Content-Type: application/json" \
  -d '{
    "prompt": "Review this diff for security and performance issues:\n\n```diff\n+    auto query = \"SELECT * FROM users WHERE id = \" + user_id;\n```",
    "workflow": "map-reduce"
  }'
```

## Example output

```
Code Review Summary:

[CRITICAL] Security — SQL Injection (line 42):
  Direct string concatenation in SQL query. Use parameterized queries.

[WARNING] Performance — Unbounded SELECT (line 42):
  SELECT * fetches all columns. Select only needed fields.

[INFO] Style — Raw SQL in business logic:
  Consider using a query builder or ORM layer.

Overall: 1 critical, 1 warning, 1 info. Block on the SQL injection fix.
```
