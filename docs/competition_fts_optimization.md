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

On the same local release build and input, repeated IK tokenization showed an
approximately 7.9% lower median runtime after the decode and dispatch changes.
This is a local microbenchmark result; the official score should be taken from
the repository's unchanged `tools/benchmark/fts_large_bench.sh` workflow on the
competition runner.  Index-build and MATCH-query scores are intentionally not
claimed without a full controlled run.

The optimization commits are:

- `490953a` — decode Unicode once during IK classification;
- `253373f` — reuse the decoded IK character across processors; and
- `ecad45f` — use contiguous IK processor dispatch.
