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

## 8.698 分回执分析与收集器优化

源码 `11b974d32c692e96c8749d1f623472a2e1e21836`：为精确内置收集器的小词数全文合并内联累计分数和取结果，保留最少匹配数、清零与回退语义。13 项全文回归、AI 向量和稀疏向量用例通过，两个新目录完整默认基准均完成，原始记录在 fts_collector_*。

相较 d07c0c9 的 11.631 分用户回执，最新九项计分耗时全部更低。仓库评分脚本/基线文件未变化；固定基线公式不能解释此种分数反向变化，而仓库旧 CI 基线对两份回执均会打满 raw 100，不能代替平台真实基线。具体未知的是平台基线、口径或运行关联，不能断言代码退化或平台存在 bug。详细条件推导及四个最新重点提交研究见 competition_fts_optimization.md 最新节。

## 2026-11-01：撤回收集器实验

0 分反馈的四类命中数正确，但九项计分耗时相对 8.698 分均增加，尤其 BENG 构建 2.522→5.728 秒。已通过 `900a6aaa97ddea6cabb929d22920b35ea37f8aec` 撤回 11b974d 的四个 C++ 文件修改，整个 src 目录恢复为 09ea627 内容，保留新增测试。重新构建及 13 项全文/AI 向量/稀疏向量回归最终通过；首个 mysqltest 连接时客户端崩溃，单项重跑通过，原始异常保留。详情见 fts_zero_score_feedback.json、fts_collector_rollback_validation.json 及优化报告最后一节。

## 6.923 分反馈后的构建并发优化

源码 `5c776bf87956dd2970c114f95dc2b69c3b8ae2c2`。在至少 4 GiB 且不少于 2 个配置/系统 CPU 的 mini mode 实例上使用两个局部索引补齐工作线程，较低容量仍单线程。完整基准总构建 38.924→25.614 秒；两个辅助补齐 SQL 从串行改为有实测重叠，工作量与校验不变。14 项全文、AI 向量和稀疏向量回归及两个低容量保护检查通过。完整查询数据仍有回退，不能推测总分。BENG 流水线试验因证据不足已撤回，保留全部测量。最新反馈在 fts_6923_feedback.json，详细验证在 fts_ddl_complement_measurements.json。

## 2026-11-01：有限构建并发取得平台 21.544 分反馈

当前稳定源码 `5c776bf87956dd2970c114f95dc2b69c3b8ae2c2`。最新 raw score 43.08772818597233，折算约 21.544 / 50 分，比前次提高约 14.621 分。总构建 16.615→8.395 秒，三个索引构建均有明显改善，命中数保持正确。平台回执按提测顺序关联，原始数据保存于 fts_21544_feedback.json。本次仅固化记录与更新提交指南，运行时代码不变；下一轮以该源码为性能及正确性对照。

## 2026-11-01：当前最佳反馈 22.508 分

源码仍为 `5c776bf87956dd2970c114f95dc2b69c3b8ae2c2`。最新 raw score 45.01618427918384，折算约 22.508 / 50 分，比前份高约 0.964 分。总构建仍为 8.395 秒，变化主要在英文/混合/LIMIT 查询；两次反馈之间无内核源码修改，不能将差异当成新优化收益。保存 fts_22508_feedback.json，更新验收指南和报告，原 21.544 分回执保留。

## 22.524 分稳定回执后的全文视图优化候选

稳定源码5c776bf最新反馈约22.524/50分，与22.508仅差约0.016分，原始回执保存在fts_22524_feedback.json。候选源码 `e9e364823387171499dea222a96bb23e6b82fccc`：为FTS开启投影/排序简化，并在严格保护条件下去掉不被外层观察的MATCH排序。LIMIT计数计划从回表/BM25 TopK变为常量输出/提前停止，固定CPU中位耗时5.508→0.345ms。20项语义回归通过，含严格读取原始引用不因裁剪消失的组合测试。平台分数待确认；原5c776bf保留为稳定对照。详细限制、计划与正负测量在competition_fts_optimization.md和fts_project_*。

## 2026-11-01：全文视图优化获得平台 32.041 分反馈

当前稳定源码为 `e9e364823387171499dea222a96bb23e6b82fccc`，分支 `competition-solutions`。最新 raw score 为 64.08181012181743，折算约 32.041 / 50 分，较前份增加约 9.517 分。LIMIT 计数从 1.8385 降至 0.1130 ms，耗时缩短约 93.85%，约快 16.27 倍，四类查询命中数未变。这与已有计划裁剪和固定 CPU 证据一致；其他三类查询仅小幅变快，分词略慢，总构建 8.495→8.101 秒的差异不全部归因于该源码。原始数据保存于 fts_32041_feedback.json，回执与源码关联以实际提测记录为准。本次只更新文档和回执，未新增源码修改或重跑测试；此前该源码的 20 项回归记录继续保留，5c776bf 为历史对照。

## 2026-11-01：同一源码收到 33.731 分反馈

最高分更新为约 33.731 / 50 分，raw score 为 67.46228119370721，较前份增加约 1.690 分。本地两次反馈之间只有文档提交，稳定源码仍为 `e9e364823387171499dea222a96bb23e6b82fccc`，基准及评分文件也未变。九个计分耗时却全部增加，其中 IK content 构建 2.826→5.528 秒、总构建 8.101→11.308 秒；固定基线与固定权重的仓库公式不能解释这一增分方向，平台实际基线、评分口径或提测关联仍待确认，不将其归因于新代码收益。LIMIT 仍为 0.1164 ms，四类命中数不变。完整回执保存于 fts_33731_feedback.json，32.041 分回执保留作较低耗时对照。本次仅更新文档，无源码改动或重复构建/回归。

## 2026-11-01：最新反馈 29.958 分，保留 33.731 分最高记录

最新 raw score 为 59.915455256790565，折算约 29.958 / 50 分，较前份下降约 3.773 分。相对 33.731 分回执，九个计分耗时却全部更低，总构建 11.308→8.499 秒，四类查询缩短约 6.79%～9.56%；固定基线、指标和权重的仓库公式不能解释此降分方向。平台实际基线、评分口径或提测关联仍未确认。三份反馈之间本地只有文档提交，当前稳定源码仍为 `e9e364823387171499dea222a96bb23e6b82fccc`，最高分仍记为 33.731。LIMIT 为 0.1085 ms，四类命中数不变。完整回执及两组对比保存于 fts_29958_feedback.json。本次检查了其余计数查询的逐文档合并路径，记录为待实测方向，没有新增源码修改、启动实例或重复构建/回归。

## 2026-11-01：30.071 分回执后实现 DaaT 布尔过滤合并

最新反馈约 30.071 / 50 分，原始回执为 fts_30071_feedback.json，仍关联此前 e9e3648，最高分保留 33.731。新候选 `acc14298157fbcc59a99e8fa95ddacb868ddead5` 在存储层合并精确的 BOOL(projected_double) 转换，保留每批首条的初始化、布尔结果及求值标记，其他表达式走原计算。旧→新→旧→新的同数据、固定 CPU 对照中，三类计数查询六组中位耗时缩短约 6.8%～7.0%，进程 CPU 缩短约 6.5%～6.6%，LIMIT CPU 不变；原始脚本查询同向改善，分词略慢，未重测构建或推算平台分数。构建和 21 项常规回归通过，包括新增多批次用例、AI 向量、完整稀疏向量和严格读取。额外二进制 PS 检查在旧版及新版都失败于首条 SQL，完整记录保留且未算通过。新候选已安装到 bin/seekdb，原始测量、全部回归和计划对照见 fts_truth_measurements.json；等待平台验证新候选。

## 2026-11-01：acc1429 收到 30.640 分反馈

最新 raw score 为 61.280532346758，折算约 30.640 / 50 分，比前份增加约 0.569 分。按提测提交与反馈顺序关联源码 `acc14298157fbcc59a99e8fa95ddacb868ddead5`，最终以平台提测记录为准。英文、中文、混合查询分别缩短约 5.04%、5.37%、4.43%，LIMIT 基本持平，命中数不变，支持前次本地交替对照的改善方向。构建和分词没有改善，不将查询百分比直接等同于总分增长。当前稳定参考更新为 acc1429，旧版 33.731 分最高记录保留；本轮九个计分耗时均低于该最高分回执，分数差异仍需平台实际基线等信息才能解释。完整数据为 fts_30640_feedback.json。本次只更新文档和回执，无新增源码或重复构建/回归；此前 21 项常规通过记录与额外 PS 检查限制保留。

## 2026-11-02：二进制 ID 比较与 ASCII 停用词本地优化

新源码为 `45831f2d7001655010dabb11d0a3084e3370b925`，尚无该源码的平台成绩。交接补充的最新成绩为 32.388 / 50 分，平台实测参考仍为 acc1429，历史最高分仍为 33.731；转录见 [反馈](fts_32388_feedback.json)，不补造未提供的 raw score 或平台 commit。实际初始 HEAD 为 053cdb6，工作区六个既有删除项保留。

本轮重新采样查询和分词后，分别减少二进制 VARCHAR 文档 ID 的通用字符集比较派发，并从内置停用词表生成只读 ASCII 字母查询表。NULL、长度及尾随空格比较规则保留；非 ASCII、标点、NUL 和跨字符集转换继续使用原集合。独立重新构建的对照及候选分别与计时二进制 SHA-256 一致。原版→仅比较器→原版→仅比较器的 CPU 13 对照，三类完整计数查询中位耗时缩短约 5.1% / 10.4% / 9.4%，进程 CPU 同向减少。停用词单独交替对照的 BENG 内部循环缩短约 20.9%，IK 略慢。

四轮原始完整基准均重新导入和构建，中文/英文/混合查询的两轮平均耗时分别缩短约 5.4% / 9.5% / 8.5%，四类命中数不变。BENG 构建两轮均快，均值缩短约 4.4%；总构建也快，但 IK 构建没有对应源码改动，不将全部差异归因于候选。端到端分词有正负波动，BENG 均值只改善约 1.4%，IK 均值慢约 7.5%，不能用内部循环数据承诺平台分词收益或总分。

24 项常规回归通过，包括 19 项全文、完整稀疏向量及四项 AI 相关测试。两个新预期来自原版二进制，已有预期未改；八个并发连接各执行 97 条分词查询，新旧所有输出摘要一致。普通协议 SQL PREPARE/EXECUTE 通过；此前 mysqltest --ps-protocol 的共同失败未改记为通过。额外二进制主键 HEX(k) 投影诊断在原版和候选均与主表扫描不一致，未修复、未计作通过，复现另存。早期辅助 SKIP_LOAD=1 原版逐组日志因重名覆盖而缺失，报告摘要仍保留，主要收益不依赖该辅助报告。全部可用正负数据、限制及 80 个原始文本文件见 [结构化测量](fts_binary_ascii_measurements.json)、[原始证据包](fts_binary_ascii_raw.json.gz) 和 [详细报告](competition_fts_optimization.md)。二进制已原子安装到 bin/seekdb，独立测试实例已关闭。

## 2026-11-02：45831f2 收到 33.682 分反馈

最新 raw score 为 67.36451884576955，综合提升率 0.33682259422884775，折算约 33.682 / 50 分，较交接中的 32.388 分增加约 1.294 分。按提测顺序关联 `45831f2d7001655010dabb11d0a3084e3370b925`，回执没有 commit 字段，最终以平台提测记录为准。相对前份，英文、中文、混合查询分别缩短约 14.34%、11.85%、13.95%，BENG 构建和分词分别缩短约 12.45%、7.51%；四类命中数不变。该方向与此前本地交替对照一致，支持将 45831f2 作为新的平台实测参考；不将不同回执的全部差异归因于源码。

历史最高分仍为 33.731，但本次九个计分耗时全部更低；同一固定正基线及权重的仓库公式不能解释“全快却略低分”，平台基线、评分版本或提测关联仍有未确认差异。保存完整 36 项指标、两份对照及计算于 [fts_33682_feedback.json](fts_33682_feedback.json)。此前 24 项常规通过记录、本地负样本、额外 PS 失败和既有主键投影问题均继续保留。下一轮先以该源码重新采样，再实施有实测收益的修改。

## 2026-11-02：后续三个内核试验均撤回

收到 33.682 分反馈后，重新采样并实际实现、构建、交替实测了固定 16 字节 ID 比较、直接 ID 数组访问和批级求值状态合并。固定长度比较的局部中文/英文中位耗时约快 1.47% / 1.53%，但四轮原始 SQL 基准的两轮平均中文约慢 0.13%、英文仅快 0.19%，方向在两对之间反转；另两个试验也没有整体稳定收益，全部撤回。保留 45831f2 为当前源码及平台实测参考，不为增加提交而合入未确认收益的代码。

原始报告、全部正负样本、三个试验补丁和汇编见 [后续测量](fts_33682_followup_measurements.json) 与 [原始证据包](fts_33682_followup_raw.json.gz)。源码已恢复并重新构建，编译产物和已安装二进制均与此前通过 24 项常规回归的版本 SHA-256 完全一致，未重复全套回归、未改动原始预期，测试实例已关闭。后续原型只做过命中检查，不记为新的全套正确性通过；额外 PS 协议限制保持不变。
