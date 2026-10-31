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

## Follow-up configuration and vector work

Snapshot `dbbc9263507151484371f7928ce84905e79fdb5d` adds bounded caches for
validated TOKENIZE defaults and decoded parser configuration, and avoids
materializing old vector LOB payloads when generating update tombstones.
New regressions cover parser alternation and validation, 2048-dimensional
out-of-row vectors, delayed embedding responses, primary-key reuse/moves,
dimension failures with exactly three requests, queue cleanup and ANN recovery.
These regressions pass. The original vector EXPLAIN comparison remains 4/15
versus 2/10; its expected output has not been changed. The latest platform FTS
score is still zero for the previous submission. The follow-up has no official
score yet; the full-text report retains both improved and regressed local timings.

## Upstream commit investigation (2026-10-30)

The live upstream ref inspected was `5f855bc118c25f24974d4e0ca4d180355442388e`.
Path-specific GitHub commit history and patches were checked for vector DML,
vector expressions, FTS, TOKENIZE and the optimizer. Relevant historical fixes
for embedding LOB handling, IK dictionary naming and generated-column filter
copies are already represented in this branch.

An experiment retaining upstream NULL delta records stalled AI publication.
A narrower variant retained NULL insert records, preserved SQL NULL and avoided
NULL LOB reads while still skipping redundant NULL deletions. It matched the
original vector test's functional output, but estimated rows/cost were 3/12
instead of 4/15. More importantly, both repeated AI vector regression suites
failed with duplicate `3-I` entries. All experimental kernel edits were reverted;
none of these variants is delivered as a fix. The submission source remains
`dbbc9263507151484371f7928ce84905e79fdb5d`. Original EXPLAIN expectations and
reported FTS score remain unresolved. See the acceptance guide for upstream
commit links and the experiment details.

After reverting and rebuilding, `vector_null_transitions`,
`vector_large_invalidation`, `ai_column_vector_regeneration` and
`ai_column_vector_races` all pass. The unchanged original vector test again
fails only the EXPLAIN 4/15 versus 2/10 comparison.

## Second upstream investigation and BENG normalization

Source snapshot `1aa6bd6e72dd7cf412833c8797224a0fbd0fdc4a` adds a UTF-8 lowercase-ASCII normalization fast path and an AI vector old-read-view regression inspired by upstream `47fa38c`. Upstream bitmap publication, DML skip invalidation, identifier quoting, child DDL error preservation and FTS index-back fixes are already present. No additional AI vector kernel fix was found.

The build and five FTS mysqltests pass, as does the new AI snapshot comparison. A 300-input corpus matches the old binary exactly. Alternating microbenchmarks show about 4.6% faster mixed English and about 2% slower uppercase/Unicode fallbacks in the repeat run; the first run was noisier. The full benchmark regressed in construction and TOKENIZE, with mixed query results. Both sets of observations are retained, not interpreted as an official score improvement. The latest reported official FTS mean improvement is -6.7624%, score zero. The original AI vector EXPLAIN mismatch remains unresolved.

## Trigger VID and FTS auxiliary sampling follow-up

Source `4b1be26c289aab054b0de50af501bbd3a74667aa` fixes vector IDs for implicit BEFORE UPDATE trigger assignments and extends HYBRID_SAMPLE to FTS document-word and bidirectional document-ID auxiliaries. Upstream `666df298` motivated tracing VID regeneration; upstream `f5674430` supplies the existing hybrid-sampling design. These are local extensions, not claims that cherry-picking either commit fixes the competition tasks.

A normal vector table with a BEFORE UPDATE copy trigger reproduced duplicate `10001-D` on the second source update in the previous binary. The new regression passes after the fix, including NULL invalidation, regeneration, rollback and unrelated-column updates. Original AI DDL, DML, worker, retry/strict and vector mysqltests all compare successfully; the original vector case passes twice without editing its expected EXPLAIN. Repeated-vector, delayed-response, snapshot, NULL-transition and large-LOB regressions also pass. NULL delta maintenance records are restored with SQL NULL payloads and safe LOB handling. Conservatively refreshing VID on vector tables with BEFORE UPDATE triggers can add index maintenance for unrelated updates; it does not create extra AI tasks.

FTS auxiliary sampling is exercised by a new parallel=6 index build/update/rollback/rebuild test, whose expected output was recorded before the patch. All six FTS comparisons pass. The default benchmark uses different parallelism, so a timing change in that benchmark cannot by itself establish a benefit from this sampling patch.

The unmodified 20,000-row benchmark ran before and after restarting the same port-11963 instance with the new binary. Reports identify checkout HEAD, while the before executable predates this patch. Both reports retain git_dirty=1 because unrelated local deletions/generated files remain; those deletions are not part of the submitted commit.

| Metric | Before | After |
| --- | ---: | ---: |
| Build IK all (s) | 13.953 | 18.290 |
| Build IK content (s) | 16.150 | 11.120 |
| Build BENG (s) | 8.317 | 5.694 |
| Build total (s) | 38.439 | 35.129 |
| TOKENIZE IK (ms) | 0.3003 | 0.2627 |
| TOKENIZE BENG (ms) | 0.1358 | 0.1486 |
| Chinese query (ms) | 4.2650 | 3.0990 |
| English query (ms) | 5.8227 | 4.7294 |
| Mixed query (ms) | 4.1369 | 3.4967 |
| LIMIT query (ms) | 4.6122 | 8.2150 |
| SELECT 1 (ms) | 0.0743 | 0.0836 |
| Load (s) | 0.792 | 0.835 |

Hits remain 8001 / 11000 / 7332 / 20. Total construction improves by about 8.6%, but IK-all construction, BENG tokenization and LIMIT regress; this pair does not establish a positive overall score or isolate causality. Raw results are [before](fts_trigger_sampling_before.txt) and [after](fts_trigger_sampling_after.txt). The latest official result, received before this patch, is mean improvement -8.7702%, score zero. A new official evaluation is still needed.

## Token scanning follow-up after official AI full score

The user confirmed the AI COLUMN implementation received full marks. The latest official FTS score remains zero: mean improvement -7.3350%, construction total 18.717 s. This feedback predates the following patch.

Source `d07c0c9ddac949bbb41cff9733df57622550a255` changes only token scanning: bypass generic character-length calls for ASCII in utf8mb4_general_ci/bin, combine whitespace/control/punctuation classification using the same charset table, and stop the all-mark filter at the first non-mark byte. Non-ASCII character-length handling and other collations retain their existing path. No benchmark, scoring, AI, or MATCH ordering code changes.

Release build succeeds. Seven FTS mysqltests pass, including 96 boundary cases recorded on the previous binary (all ASCII bytes, NUL, delimiter runs, long tokens, Unicode, and three collations). The original AI vector test also passes.

Microbenchmark: SELECT BENCHMARK(30000, TOKENIZE(@doc, 'beng')), one warm-up then six measured client calls; median wall time. The mixed input is the full benchmark's BENG text; uppercase input is `UPPERCASE DATABASE PERFORMANCE INDEXING AUDIT WORD WORD`; Unicode input is `中文数据库 café CAFÉ İstanbul Straße Σ σ 😀`; long input repeats `abcdefghijklmnopqrstuvwxyz ` 100 times. Input is assigned via CONVERT(hex USING utf8mb4). Initial run order is old/new; repeat order is new/old and excludes the long case.

| Input | First old/new (s) | Repeat old/new (s) |
| --- | ---: | ---: |
| Mixed English | 0.805546 / 0.503018 | 0.804849 / 0.473183 |
| Uppercase | 0.456366 / 0.439789 | 0.459285 / 0.272578 |
| Unicode | 0.418033 / 0.540586 | 0.276361 / 0.260212 |
| Long English | 8.797956 / 4.974280 | Not repeated |

The first Unicode regression did not reproduce. Large between-run differences remain; both runs are retained in `token_scan_micro_before.json`, `token_scan_micro_after.json`, `token_scan_micro_before_repeat.json`, and `token_scan_micro_after_repeat.json`.

The unmodified full benchmark uses 20,000 rows, 3,000 tokenization rounds, 200 query rounds, three samples and 30 warmups, on the same port-11963 instance before/after binary restart:

| Metric | Before | After |
| --- | ---: | ---: |
| IK all build (s) | 19.054 | 21.180 |
| IK content build (s) | 15.950 | 14.433 |
| BENG build (s) | 10.016 | 8.904 |
| Total build (s) | 45.038 | 44.545 |
| IK TOKENIZE (ms) | 0.3019 | 0.2881 |
| BENG TOKENIZE (ms) | 0.1389 | 0.1292 |
| Chinese query (ms) | 5.4623 | 5.3593 |
| English query (ms) | 8.3770 | 7.0861 |
| Mixed query (ms) | 6.1255 | 3.8456 |
| LIMIT query (ms) | 8.1734 | 5.8310 |
| SELECT 1 (ms) | 0.0861 | 0.0800 |
| Load (s) | 0.816 | 0.867 |

Hit counts remain 8001 / 11000 / 7332 / 20. Raw reports: [before](fts_token_scan_before.txt), [after](fts_token_scan_after.txt). BENG tokenization improves about 7.0%, BENG construction about 11.1%, total construction about 1.1%; IK-all build regresses. After-run English query samples span 5.7393–8.0291 ms, so query improvements cannot all be attributed to a per-character tokenizer change. This is evidence for submitting the patch, not proof of a positive official score. Original benchmark files and expected outputs remain unchanged. The before report identifies checkout 6fbf19e while the before executable is source 4b1be26 (subsequent changes were documentation only). The after executable is d07c0c9; git_dirty reflects preserved unrelated local files/deletions.

## IK follow-up after official 11.631 / 50

The latest user-reported official result for `d07c0c9ddac949bbb41cff9733df57622550a255` is 11.631 / 50, mean improvement 0.11630917273202195 and raw score 23.26183454640439. AI COLUMN already has full marks. Source `97112d83904fa5f2236e146266bc7c3ec45d63ba` is a new candidate, not a newly verified official score.

Delivered changes:

- For utf8mb4_general_ci/bin words composed entirely of well-formed basic CJK U+4E00–U+9FFF, copy the word into the existing allocator without an identity case conversion. Other words/collations retain the converter. Key ownership and lifetime remain unchanged.
- Remove per-parser acquisition/construction of the unused IK stop-word dictionary. Its token-filter call was already commented out. Shared add-word stop-word filtering is preserved.

Upstream [2d6a632](https://github.com/oceanbase/seekdb/commit/2d6a63248d14b65ae0ea959a7dc266512094d7c5) disables additional defensive checks by default. An isolated 1/0/0/1 diagnostic did not show stable improvement, so this configuration patch was not adopted and the instance was restored to 1. The central changes in upstream [0ff2745](https://github.com/oceanbase/seekdb/commit/0ff2745716123f0e86a137e0c88861b1841ff7a9) are already present. The delivered patch is local work, not a claimed cherry-pick.

Experiments with binary dictionary-range lookup and shared character decoding were reverted. The combined candidate regressed strongly even on a fixed logical CPU; default smart-mode gains from decoding alone were inconclusive. No rejected experiment remains in source.

The optimized RelWithDebInfo build and nine mysqltests pass: eight FTS suites, including 320 new IK compatibility cases, and the original AI vector-index test. The cases cover both modes, three collations, overlaps, supplementary Unicode, control bytes, CJK range boundaries and retained stop words. The first 314 expected outputs were captured on the previous scored binary; six extra CJK-boundary cases were captured before the case-conversion change, and the existing prefix was verified identical. Original competition expectations are untouched.

All diagnostics are retained in [measurements](fts_ik_case_measurements.json). Under normal scheduling the microbenchmarks were noisy. For the controlled follow-up, old and new instances ran sequentially on logical CPU 13, each executing BENCHMARK(10000,TOKENIZE(@doc,'ik',options)), with one warm-up and three measured calls. Comparing the final old-version repeat to the delivered candidate:

| Input / mode | Old median wall s | New median wall s |
| --- | ---: | ---: |
| Mixed / smart | 0.889652 | 0.862526 |
| Overlap / smart | 0.483025 | 0.460324 |
| Mixed / max_word | 0.943401 | 0.877716 |
| Overlap / max_word | 0.581692 | 0.505133 |

Full unmodified benchmark, restored normal CPU scheduling, same instance configuration and 20,000 rows / 3,000 tokenization rounds / 200 query rounds / three samples / 30 warmups:

| Metric | Before | After |
| --- | ---: | ---: |
| IK all build (s) | 15.721 | 15.075 |
| IK content build (s) | 11.723 | 12.434 |
| BENG build (s) | 9.101 | 6.888 |
| Build total (s) | 36.563 | 34.414 |
| IK TOKENIZE (ms) | 0.2919 | 0.2831 |
| BENG TOKENIZE (ms) | 0.1392 | 0.1323 |
| Chinese query (ms) | 5.4170 | 4.1227 |
| English query (ms) | 6.3556 | 6.0525 |
| Mixed query (ms) | 4.5396 | 4.6016 |
| LIMIT query (ms) | 4.4854 | 6.1377 |
| SELECT 1 (ms) | 0.0778 | 0.0933 |
| Load (s) | 0.791 | 0.819 |

Hits remain 8001 / 11000 / 7332 / 20. Reports: [before](fts_ik_case_before.txt), [after](fts_ik_case_after.txt). The before report's original label is ik_dictionary_before, from the initial investigation; its executable is the previously scored d07c0c9. The after executable is 97112d8. Reports retain unrelated git_dirty state.

IK tokenization is about 3.0% lower and total construction about 5.9% lower in this pair. However IK-content construction, mixed queries, LIMIT and SELECT 1 regress; the new LIMIT standard deviation is 1.0013 ms. BENG construction improves even though its implementation was unchanged, further showing that all observed timing differences cannot be credited to this patch. Official re-evaluation is required to determine whether the total score improves. The previous scored source remains available for rollback.


## Latest FTS follow-up: concrete small-term merger dispatch

Source `3c126f77da5836579e95111982ed0c00b6bae04b` reduces per-posting dispatch in the existing small-term merger and bypasses empty filter calls. AI COLUMN source remains unchanged and officially full-marked. Nine FTS regressions, the original sparse-vector query suite and original AI-vector suite pass. Batch-output rewriting was rejected after a LIMIT regression. Pinned query medians improve by approximately 0.44–3.27%, with shared-host limitations and outliers; no official score is claimed. The latest platform feedback remains 5.098 / 50, and its lower score despite all scored latencies improving requires an unavailable platform baseline to reconcile. See [FTS report](competition_fts_optimization.md) and [submission guide](比赛成果验收与提交指南.md) for exact source, measurements and limitations.


## October 31: invocation-private FTS state

Source `5d89f4f86f6a23611b8dae5c24ebd71448418c2c` replaces the private IK arbitration hash map with an ordered inline array and removes bucket latches from invocation-private token-frequency maps. All five map-instantiation sites were inspected and normalized to the shared alias; shared dictionary and stop-word synchronization remains intact. This follows additional upstream container/lifecycle investigation and qualitative GDB sampling. The dictionary-allocator and character-code-cache experiments were rejected and reverted.

The optimized build, ten FTS suites and the original AI vector test pass. Eight concurrent connections produce 2,240 outputs matching the old binary. Controlled expression microbenchmarks improve roughly 1.9–10.7% for IK and 19.4–19.7% for BENG, but the full default SQL benchmark has regressions including BENG tokenization and LIMIT. No official improvement is claimed for this source. Latest official feedback is 5.306 / 50 for the preceding submission; AI remains full-marked. Full measurements and submission details are in the [FTS report](competition_fts_optimization.md) and [submission guide](比赛成果验收与提交指南.md).

A follow-up paired SQL-loop run, reusing the same indexes with SKIP_LOAD=1 and five samples, improves IK/BENG by 13.23% / 19.08%. SELECT 1 also improves and old-version variance is high, so this does not invalidate the full default-run regressions or establish an official score. Both raw diagnostic reports are linked in the FTS report.

## 2026-10-31 继续检索官方提交及节点池优化

新增候选源码 `e047eadaccc4ad42ce65ec9f001b25e65e65b4c8`，包含官方 `51d5ab1` 原生预处理格式化消除（本地 `8815166`）及私有分词节点池去锁。检索最近 300 条官方历史，已实现/过于宽泛/不属于热路径的提交未重复引入。11 项全文回归、AI 向量索引及并发输出一致性通过。

性能结论有限：BENG SQL 循环改善，其他本地数据有回退与明显波动；新完整构建基准遭遇测试库删除锁冲突，只有 SKIP_LOAD 复测，不能宣称整体提速或预估官方分数。细节、官方提交链接、客户端协议检查限制及原始正负数据均在 competition_fts_optimization.md 最新节和 fts_node_pool_* 文件中。

## 5.591 分后的全文 posting 消费优化

候选源码 `09ea627e18a1ed22ac6d916290da6e596d0ff18f`。撤回节点池去锁实验，新增小词数全文 DaaT 的缓存 posting 合并读取；多词与向量通用路径保持原样。12 项全文回归、AI 向量及完整稀疏向量查询通过。研究了八类关键词的 37 条去重命中，并逐文件比对八个重点官方提交，比较表见 competition_fts_optimization.md 最新节。

两个新目录均跑完原始完整基准，修复了上轮缺少完整构建对比的问题；但完整查询回退与固定 CPU 对照改善并存，不能宣称整体提速。原始数据在 fts_posting_* 文件中，不修改评分脚本或计算模拟平台得分。
