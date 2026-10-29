# 2026 seekdb competition implementation report

This branch collects the implementations and local verification completed for
the competition tasks described in `ob.md`.  It intentionally records tested
behavior separately from partial AI-column work so that reviewers can reproduce
the results without treating compilation alone as proof of correctness.

## Task 1: local deployment

- The source tree builds a runnable `seekdb` binary.
- The deployed instance was connected with the MySQL-compatible client and used
  for all end-to-end tests below.
- Telemetry ID obtained from the deployed instance:
  `80feb7ed-3e67-8970-9a8c-ee6d6ef08b01`.
- Submission to the competition platform remains an account-owner action.

## Task 2: competition issue fix

- Issue branch: `fix-issue-1489`.
- Commit: `368e211`.
- The branch is present in the configured fork. Creating the upstream pull
  request and submitting its URL remain account-owner actions.

## AI function enhancement

Implemented and exercised against the local deterministic mock service:

- JSON and LOB values in `AI_PROMPT`, nested prompts, repeated placeholders,
  Unicode, escaping, and invalid placeholder diagnostics.
- Default and explicit embedding dimensions, response element validation,
  NULL propagation, multi-row batches, and response-to-input index recovery.
- OpenAI completion request configuration and response validation.
- Ollama completion and embedding protocols, case-insensitive provider names,
  and endpoints with an empty access key.
- Rerank string/object inputs, stable global indexes across batches, and result
  validation.

Manual end-to-end verification covered completion configuration, embedding
dimensions 4 and 768, batched NULL mapping, rerank identifiers, and both Ollama
protocol variants.

## Native file SQL

Implemented native SQL-engine scans for:

- CSV and JSONL with schema inference, diagnostics, configurable CSV syntax,
  BOM handling, record-size limits, file-change detection, and numeric
  precision preservation.
- Parquet footer parsing, row groups, nullable and temporal types, plain,
  dictionary and fixed-width values, INT96 timestamps, and projected-column
  pruning.
- `read_csv`, `read_json`/`read_ndjson`, `read_parquet`, `glob`, dynamic
  `DESCRIBE`, and `COPY ... TO` CSV.
- secure path checks and the `enable_file_sql` feature gate.

End-to-end tests exercised filters, projections, aggregation, ordering, limits,
CSV/JSONL/Parquet reads, COPY readback, glob discovery, and same-/cross-format
joins through seekdb's native plans.

## AI COLUMN

The following behavior is implemented and locally verified:

- `CREATE TABLE` and `ALTER TABLE ... ADD COLUMN` grammar for asynchronous AI
  columns, option/type/function/vector validation, and rejection of chained AI
  dependencies.
- Persistent current/history metadata including expression, dependency IDs,
  hidden state/request/spec columns, retry configuration, and definition
  version.
- `SHOW AI COLUMNS` backed by persistent metadata.
- Protection against direct INSERT/UPDATE assignment and ordinary
  DROP/MODIFY/CHANGE/default alterations of AI result/state columns.
- Metadata and pending-task cleanup during permanent table drop.
- Automatic INSERT task creation with one request group per changed row.
- Dependency-aware invalidation on UPDATE; unrelated columns do not enqueue
  work, while primary-key changes move the durable row predicate.
- DELETE cleanup, multi-row trigger execution, and transaction semantics:
  task creation/deletion rolls back with the business DML.

The DML path is implemented with internal row triggers installed together with
new AI-column tables. Tests verified exact numeric predicates such as
`` `id` = 2 ``, grouped request IDs, dependency invalidation, and zero tasks
after rollback or row deletion.

Not claimed complete: a production-safe tenant-aware asynchronous model worker,
manual `RETRY AI COLUMN`, strict-read enforcement, and trigger regeneration for
AI columns added to an existing table. An experimental timer worker was
discarded after testing showed that its system internal connection could not
see business-tenant task rows; no nonfunctional implementation is retained.

## Full-text performance

The IK hot path decodes Unicode once and uses contiguous processor dispatch,
removing repeated classification work without changing tokenization semantics.
The complete official-size local benchmark was run with 20,000 rows, batch 500,
3,000 tokenization rounds, 200 query rounds, three samples, 30 warmups, and a
fresh load. The supplied score tool accepted every metric and reported:

- mean improvement: 72.46%
- performance score: 100.00/100

Detailed timings and correctness notes are in
`docs/competition_fts_optimization.md`.

## Build and repository state

The final tracked source builds with:

```bash
cmake --build build_release --target seekdb -j4
```

Generated parser/schema outputs and deployment artifacts are deliberately not
committed. The competition implementation is on `competition-solutions`; a
network timeout prevented pushing this accumulated branch during the final
verification, so it must be pushed when GitHub connectivity is available.
