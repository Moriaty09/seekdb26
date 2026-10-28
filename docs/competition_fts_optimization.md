# Full-text indexing optimization notes

This document describes the full-text tokenizer changes made for the 2026
competition.  The benchmark inputs, SQL, scoring script, and fixed baseline are
not modified by these changes.

## Bottleneck

Profiling the IK tokenizer's character-processing loop showed that the same
UTF-8 character was decoded repeatedly while the tokenizer dispatched it to
letter, CJK, and quantifier processors.  Processor dispatch also walked a
pointer-oriented structure on every character.  These costs occur both while
building an IK full-text index and while evaluating `TOKENIZE(..., 'ik')`.

The relevant hot path is in `src/storage/fts/ob_ik_ft_parser.cpp` and the IK
character classifier in `src/storage/fts/ik/ob_ik_char_util.h`.

## Changes

The implementation now:

1. decodes a UTF-8 code point once and reuses the decoded value for all IK
   character classifications;
2. passes that decoded character through the processor chain instead of asking
   individual processors to decode the same input again; and
3. stores processor dispatch entries contiguously, reducing pointer chasing in
   the per-character loop.

The changes do not alter dictionaries, token boundaries, stop-word handling,
processor order, or emitted token frequencies.

An attempted BENG optimization is intentionally not part of the submission.
BENG's downstream word collector retains each emitted token pointer, so both a
view into the analyzer's transient buffer and a single reusable copy buffer
change previously emitted results.  BENG therefore continues to allocate
stable storage for every token.

## Correctness validation

IK output from the original and optimized implementations was compared token
by token for the benchmark's Chinese and mixed-language inputs.  The token
strings, order, lengths, and frequencies were identical.  The optimized
release build also completed normal `TOKENIZE` SQL execution successfully.

The BENG lifetime constraint was verified with real SQL, not inferred from the
interface: buffer reuse produced overwritten earlier words and was reverted
before committing.  This preserves BENG behavior while keeping the independent
IK optimization.

## Local performance result

The unchanged competition workflow was run against the release build with its
full scoring configuration (`ROWS=20000`, `BATCH=500`, `ROUNDS=3000`,
`QUERY_ROUNDS=200`, `SAMPLES=3`, `WARMUP=30`, and `SKIP_LOAD=0`).  The generated
report was accepted by the unchanged scoring script and produced:

| Category | Metric | Local result | Fixed baseline | Improvement |
| --- | ---: | ---: | ---: | ---: |
| Build | IK title + content | 8.441 s | 35.2836 s | 76.08% |
| Build | IK content | 6.385 s | 28.3764 s | 77.50% |
| Build | BENG English | 3.272 s | 14.7578 s | 77.83% |
| Tokenize | IK average | 0.2578 ms | 0.76478 ms | 66.29% |
| Tokenize | BENG average | 0.1659 ms | 0.42262 ms | 60.74% |
| Query | Chinese | 3.5683 ms | 16.6628 ms | 78.59% |
| Query | English | 5.1754 ms | 24.3042 ms | 78.71% |
| Query | Mixed | 3.9786 ms | 17.5593 ms | 77.34% |
| Query | LIMIT | 4.4994 ms | 16.2334 ms | 72.28% |

The category improvements were 77.13% for build, 63.52% for tokenization, and
76.73% for queries.  The overall local improvement was 72.46%, which maps to
`100.00 / 100` under the repository's fixed scoring formula.  Correctness
signals were also retained: the Chinese, English, mixed, and LIMIT queries
reported 8001, 11000, 7332, and 20 hits respectively.

These values are a complete local competition-parameter run rather than a
microbenchmark.  The official result remains the score obtained on the
competition runner because hardware and machine load differ between
environments.

The optimization commits are:

- `490953a` — decode Unicode once during IK classification;
- `253373f` — reuse the decoded IK character across processors; and
- `ecad45f` — use contiguous IK processor dispatch.
