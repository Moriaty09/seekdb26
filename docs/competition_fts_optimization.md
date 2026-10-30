# Full-text indexing optimization and validation

The final implementation is in commit
`f58e48b87e0340ecda949ecfbdad6fbd72a6a651`. Local functional regression tests
pass, including the new retrieval path that omits unobserved BM25 scores.
**The available measurements do not establish a stable overall competition
score.** This document replaces the earlier comparison against the checked-in
CI baseline: different machines cannot be used to attribute a speedup to these
changes, and the previous “100 / 100” statement was not an official result.

Neither the benchmark workload, its generator, its scoring script, nor its
checked-in baseline was modified.

## Implementation

The previously retained IK changes decode each character once and pass its
classification through a contiguous processor dispatch array. This revision
adds three tokenizer changes:

- Same-charset stopword lookup avoids constructing an arena per token, while
  retaining the stopword collation for hashing and comparison. Cross-charset
  input keeps the conversion path.
- Token frequency accumulation uses one hash lookup instead of a get followed
  by an overwrite. First-occurrence and duplicate-frequency behavior is retained.
- IK's active CJK dictionary prefixes use an inline array with stable compaction.
  Previously active prefixes are processed in their original order before a new
  prefix is appended. Array growth uses the existing allocator. Dictionary-hit
  assignment also copies the dictionary pointer.

The retrieval optimization is deliberately conservative. A primary text
retrieval scan may omit numeric score calculation only when a SELECT has a
standalone positive boolean MATCH filter and every semantic reference to that
MATCH is the same direct boolean conversion. It applies only to ordinary
natural-language mode without boosts or child statements. Numeric comparisons,
score projections and aggregates, explicit and implicit relevance ordering,
NOT/OR wrappers, boolean-search mode, and lookup/index-merge consumers retain
scoring. Parameterized thresholds therefore retain their normal scoring and
plan-cache behavior.

The complete semantic expression list is checked, including the implicit
relevance ORDER BY added during preprocessing. `EXPLAIN` confirms
`calc_relevance=false` for the benchmark COUNT filters and
`calc_relevance=true` for relevance-ordered LIMIT and score projection queries.

The storage changes make that path executable: legacy iterator setup skips
absent score-statistics definitions; DaaT/TaaT skip BM25 estimator initialization
when no score expression exists; posting iterators still initialize their batch
skip storage; and membership projection remains positive for matching postings.
TaaT initializes a spill/hash partition even without corpus statistics and
coalesces repeated document IDs. These changes address previously unused paths;
simply changing the optimizer flag caused a null dereference and was insufficient.

BENG continues to allocate stable token storage. Reusing an analyzer's transient
buffer is unsafe because the downstream collector retains token pointers.

## Correctness evidence

The following mysqltest cases passed against the final implementation:

- Existing `fts_index/t/tokenize_function.test`.
- New `fts_index/t/tokenize_hotpath.test`: repeated token counts, Unicode text,
  case/accent handling, UTF8MB4 collations, overlapping dictionary prefixes, and
  both IK modes.
- New `fts_index/t/match_filter_relevance.test`: positive and negative filters,
  OR, empty results, forced primary-index lookup, numeric scores and SUM,
  implicit and explicit ordering/LIMIT, a prepared threshold changing
  `0 → 2 → 0` with results `3 → 1 → 3`, boolean search, and a query with over
  256 distinct terms exercising TaaT. The long-query optimized and scored
  reference forms both return two rows.

An additional differential run compared 400 queries with the pre-change binary:
150 deterministic random combinations drawn from the built-in dictionary and
50 mixed Chinese/English/numeric/quantifier/emoji combinations, each in smart
and max-word mode. The complete output matched byte for byte.

All completed benchmark runs preserved the expected Chinese, English, mixed,
and LIMIT hit counts: **8001, 11000, 7332, and 20**. These are local results;
the external competition runner has not been rerun by this validation.

## Measurements on 2026-10-30

The unchanged benchmark used `ROWS=20000`, `BATCH=500`, `ROUNDS=3000`,
`QUERY_ROUNDS=200`, `SAMPLES=3`, `WARMUP=30`, and `SKIP_LOAD=0`. The isolated
instance used port 11962, a 4 GB memory budget, a 2 GB data file, and a 2 GB log
disk. Before/after runs reused the same instance directory and rebuilt the
benchmark table and indexes through the standard script.

The final adjacent pair fixed the server's CPU affinity to `0,2,4,6` and the
SQL client's affinity to `8` for both binaries. Other users' processes were
not changed. Our AI test instances and compilation jobs were stopped for this
pair. CPU affinity did not eliminate shared-host variability.

| Metric | Before | Final implementation |
| --- | ---: | ---: |
| Build IK title + content | 19.516 s | 17.031 s |
| Build IK content | 12.827 s | 11.219 s |
| Build BENG English | 5.888 s | 5.690 s |
| Build total, auxiliary metric | 38.253 s | 33.964 s |
| TOKENIZE IK mean | 0.2714 ms | 0.2776 ms |
| TOKENIZE BENG mean | 0.1452 ms | 0.1612 ms |
| Chinese query mean | 5.9557 ms | 3.1551 ms |
| English query mean | 7.9280 ms | 5.5778 ms |
| Mixed query mean | 6.0516 ms | 5.9719 ms |
| LIMIT query mean | 5.9366 ms | 7.8155 ms |
| SELECT 1 mean, auxiliary metric | 0.0913 ms | 0.0863 ms |

Raw reports, including sample standard deviations, are retained as
[before](fts_benchmark_20261030_before.txt) and
[after](fts_benchmark_20261030_after.txt).
The reports' `git_head` field describes the working tree when the script ran,
not the source identity of an already copied executable. The before executable
was saved from the running pre-change instance before this revision's changes;
the after executable was copied from the integrated build containing the final
FTS changes. Both executable snapshots remain under `/tmp/seekdb-fts-*-binary`
on the validation server.

There are measured regressions as well as improvements in this pair: both
TOKENIZE means and the LIMIT mean are worse. They must not be omitted from the
comparison. Earlier runs of the same pre-change binary had build times around
14.3/11.3/5.9 seconds and query means around 3.5/5.3/4.0/4.6 milliseconds.
During the final pair the old English query samples ranged from 6.36 to
9.43 milliseconds; its LIMIT samples ranged from 4.64 to 7.27 milliseconds.
The new English query samples ranged from 4.58 to 7.37 milliseconds. Another
user's large index-building workload was active during this period. This is
strong evidence of environmental interference, but is not a justification for
claiming every regression is unrelated to the changes.

Before adding score omission, two complete local pairs showed modest,
repeatable index-build improvements from the tokenizer changes: content IK
11.329 → 11.118 and 11.521 → 11.126 seconds, and BENG 5.888 → 5.687 and
5.989 → 5.788 seconds. TOKENIZE and query timings fluctuated across those pairs;
they did not establish an overall positive score by themselves.

The first IK build was also investigated through DDL logs. In the initial local
baseline its dictionary stage took approximately 2 ms and auxiliary mapping
creation about 0.4 s, while the index/data-complement work dominated the
14.285 s build. The reported first-index slowdown therefore cannot simply be
attributed to SQL dictionary loading.

## Acceptance and remaining limits

The functional evidence supports submitting the implementation for official
validation. The local timing evidence supports further measurement, not a
promised score: rerun the same before/after pair on an otherwise idle machine
or use the competition runner's result. A result obtained by comparing this
machine with the repository's historical CI baseline must not be reported as
an optimization gain or a competition score.

The isolated port-11962 validation instance was stopped after testing. Its data
and logs were retained for inspection; other deployments were left alone.
