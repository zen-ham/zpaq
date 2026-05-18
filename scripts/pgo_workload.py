"""
PGO training workload for the zpaq binding.

Goal: exercise the hot codepaths the optimizer will see in real usage,
across data types and compression levels, without spending so long that
CI build time becomes painful. Targets ~5-15 seconds total runtime on a
modern CPU.

We don't care about the OUTPUT of compress/decompress here, only that
the JIT-emitted predictor + the libzpaq inner loops + our parallel
decompress workers all get exercised so the profile counters reflect
real-world branch frequencies.
"""
import os
import random
import sys
import time

import zpaq


def gen_text(n_bytes: int) -> bytes:
    """English-ish lorem with repetition - exercises text-detected path."""
    words = [
        b"the ", b"a ", b"and ", b"of ", b"to ", b"in ", b"that ", b"is ",
        b"was ", b"he ", b"for ", b"it ", b"with ", b"as ", b"his ", b"on ",
        b"be ", b"at ", b"by ", b"i ", b"this ", b"had ", b"not ", b"are ",
        b"but ", b"from ", b"or ", b"have ", b"an ", b"they ", b"which ",
    ]
    rng = random.Random(42)
    out = []
    n = 0
    while n < n_bytes:
        w = rng.choice(words)
        out.append(w)
        n += len(w)
    return b"".join(out)[:n_bytes]


def gen_binary(n_bytes: int, seed: int = 7) -> bytes:
    """Pseudo-random bytes - exercises the incompressible path."""
    return random.Random(seed).randbytes(n_bytes)


def gen_repetitive(n_bytes: int) -> bytes:
    """Highly repetitive data - exercises LZ77 / dedup paths."""
    base = b"REPEATING PATTERN " * 64  # ~1KB pattern
    return (base * ((n_bytes // len(base)) + 1))[:n_bytes]


def gen_mixed(n_bytes: int) -> bytes:
    """Text + binary mix - exercises the predictor's adaptation across types."""
    text = gen_text(n_bytes // 2)
    binary = gen_binary(n_bytes - len(text), seed=11)
    return text + binary


def main() -> int:
    print("zpaq PGO training workload starting...")
    t0 = time.perf_counter()

    samples = [
        ("text",        gen_text(512_000)),
        ("binary",      gen_binary(256_000)),
        ("repetitive",  gen_repetitive(512_000)),
        ("mixed",       gen_mixed(512_000)),
    ]

    # Exercise compress at every level, single and multi-thread, with and
    # without dedup. Decompress every output. The PGO collector samples
    # branch frequencies inside this loop.
    for label, data in samples:
        for level in (1, 3, 5):
            for threads in (1, 0):
                blob = zpaq.compress(data, level=level, threads=threads)
                out = zpaq.decompress(blob, threads=threads)
                assert out == data, f"{label}/L{level}/t{threads}: mismatch"
            # Dedup path
            blob = zpaq.compress(data, level=level, dedup=True)
            out = zpaq.decompress(blob)
            assert out == data, f"{label}/L{level}/dedup: mismatch"

    elapsed = time.perf_counter() - t0
    print(f"workload done in {elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
