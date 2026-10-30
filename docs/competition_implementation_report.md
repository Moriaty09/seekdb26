# 2026 seekdb competition implementation report

This branch collects the implementations and local verification completed for
the competition tasks described in `ob.md`.  It intentionally records tested
behavior separately from outstanding evaluation differences so that reviewers can reproduce
the results without treating compilation alone as proof of correctness.

## Task 1: local deployment

- The source tree builds a runnable `seekdb` binary.
- The deployed instance was connected with the MySQL-compatible client and used
  for all end-to-end tests below.
- The participant confirmed that task 1 passed the platform's UUID check.
  Preserve the accepted deployment UUID; isolated regression instances have
  different identities and must not replace it.

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

The October 30 update adds durable asynchronous claiming and lease recovery,
request/version checks, cached typed results, bounded retries, and atomic
publication of mixed READY/FAILED column groups. External model calls occur
outside the business transaction and only after its committed task is visible.
`ALTER ... RETRY AI COLUMN` supports status and business predicates. Strict
reads check actual referenced columns on every execution, including cached
plans, optimized-away expressions, views, and uncommitted caller changes.
ALTER ADD installs triggers and backfills existing rows. Direct assignment
protection also covers INSERT SET, implicit columns, duplicate-key updates,
and updatable views.

Additional regressions cover mixed successful/failed groups, READY SQL NULL,
conversion retries without repeating cached successes, expired leases, quoted
identifiers, ALTER ADD backfill, repeated NULL/vector transitions, and rollback.
See the Chinese acceptance guide for final test outcomes and submission commit.

The original vector-index test still differs in EXPLAIN estimated rows/cost
(expected 4/15, observed 2/10). Its data and ANN checks match. The original
expected output is preserved; the test is not reported as passing. The same
estimate behavior is reproducible with ordinary vector columns and depends on
delta-entry history, so this difference needs evaluator clarification.

## Full-text performance

The update reduces redundant tokenizer conversion/hash work and skips BM25
calculation only for positive natural-language MATCH filters that do not
consume scores. Score projections, thresholds, sorting, complex boolean
expressions, and lookup/merge paths retain scoring. Differential tokenization
and SQL regression checks preserve existing results.

The earlier 100/100 local score against a fixed external baseline did not
establish a code improvement and is withdrawn. Use paired before/after runs
on the same machine, with identical benchmark data and parameters. The final
measurements and their limits are recorded in
[the full-text report](competition_fts_optimization.md). Platform performance
scores require a new evaluation of the submitted commit.

## Build and repository state

The final tracked source builds with:

```bash
cmake --build build_release --target seekdb -j4
```

Generated parser/schema outputs and deployment artifacts are not committed.
The target fork is `Moriaty09/seekdb26`, branch `competition-solutions`.
For the verified submission commit, test results, and remaining participant
steps, consult [the acceptance guide](比赛成果验收与提交指南.md).
