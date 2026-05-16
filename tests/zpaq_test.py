"""Smoke tests for zpaq. Named *_test.py per user file-naming preference."""
import os
import random

import pytest

import zpaq


def _roundtrip(data, level):
    blob = zpaq.compress(data, level=level)
    assert isinstance(blob, bytes)
    decoded = zpaq.decompress(blob)
    assert decoded == data


def test_empty_roundtrip():
    _roundtrip(b"", 1)


@pytest.mark.parametrize("level", [0, 1, 2, 3, 4, 5])
def test_levels_roundtrip(level):
    _roundtrip(b"hello world " * 200, level)


def test_random_roundtrip():
    rng = random.Random(0xC0FFEE)
    payload = bytes(rng.getrandbits(8) for _ in range(50_000))
    _roundtrip(payload, 3)


def test_compression_actually_shrinks_repetitive_data():
    payload = b"A" * 100_000
    blob = zpaq.compress(payload, level=3)
    assert len(blob) < len(payload) // 10, (
        "Highly repetitive data should compress to <10% of its size"
    )


def test_bad_level_raises():
    with pytest.raises(ValueError):
        zpaq.compress(b"x", level=6)
    with pytest.raises(ValueError):
        zpaq.compress(b"x", level=-1)


def test_decompress_garbage_raises():
    with pytest.raises(zpaq.Error):
        zpaq.decompress(b"this is not a zpaq stream")


def test_memoryview_accepted():
    payload = bytearray(b"hello world " * 50)
    blob = zpaq.compress(memoryview(payload), level=1)
    assert zpaq.decompress(blob) == bytes(payload)
