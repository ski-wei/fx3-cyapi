import gc
import threading
import time

import numpy as np
import pytest
from fx3_cyapi import AcquisitionConfig, AcquisitionEngine


def config(**kwargs):
    result = AcquisitionConfig()
    result.block_size = 4096
    result.queue_depth = 4
    result.queue_capacity = 4
    result.pool_extra = 2
    for key, value in kwargs.items():
        setattr(result, key, value)
    return result


@pytest.fixture
def engine():
    with AcquisitionEngine() as engine:
        engine.open(simulated=True)
        engine.configure(config())
        yield engine


def wait_until(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    pytest.fail("Condition did not become true")


def test_data_pattern_readonly_and_sequence(engine):
    engine.start()
    for _ in range(6):
        block = engine.read_block(1000)
        assert block is not None
        view = np.frombuffer(block, dtype=np.uint8)
        expected = ((np.arange(block.size, dtype=np.uint64) + block.sequence) % 256).astype(np.uint8)
        np.testing.assert_array_equal(view, expected)
        assert memoryview(block).readonly
        assert not view.flags.writeable
        assert block.timestamp_ns > 0
        with pytest.raises(ValueError):
            view[0] = 0
    engine.stop()
    assert not engine.stats().running


def test_queue_bound_and_drops(engine):
    engine.start()
    wait_until(lambda: engine.stats().dropped_blocks > 10)
    stats = engine.stats()
    assert stats.queued_blocks == 4
    assert stats.queue_high_water == 4
    assert stats.pool_blocks == 10
    assert stats.pool_bytes == 40960
    assert stats.dropped_bytes == stats.dropped_blocks * 4096


def test_retained_view_never_overwritten_or_freed():
    engine = AcquisitionEngine()
    engine.open(simulated=True)
    engine.configure(config())
    engine.start()
    block = engine.read_block(1000)
    view = np.frombuffer(block, dtype=np.uint8)
    original = view.copy()
    del block
    time.sleep(0.05)
    engine.close()
    np.testing.assert_array_equal(view, original)
    with pytest.raises(RuntimeError, match="Release outstanding"):
        engine.open(simulated=True)
    del engine
    gc.collect()
    np.testing.assert_array_equal(view, original)


def test_destructor_stops_active_worker_without_invalidating_view():
    engine = AcquisitionEngine()
    engine.open(simulated=True)
    engine.configure(config())
    engine.start()
    block = engine.read_block(1000)
    view = memoryview(block)
    original = bytes(view)
    del block, engine
    gc.collect()
    assert bytes(view) == original


def test_exported_views_cause_bounded_drops(engine):
    engine.start()
    held = []
    for _ in range(6):
        block = engine.read_block(1000)
        assert block is not None
        held.append(memoryview(block))
    originals = [bytes(v) for v in held]
    wait_until(lambda: engine.stats().dropped_blocks > 0)
    assert engine.stats().pool_blocks == 10
    assert [bytes(v) for v in held] == originals
    engine.stop()
    with pytest.raises(RuntimeError, match="Release outstanding"):
        engine.start()
    held.clear()
    del block
    gc.collect()
    engine.start()
    engine.stop()


def test_stop_wakes_reader_and_releases_gil(engine):
    engine.configure(config(simulated_interval_ms=10000, timeout_ms=20000))
    engine.start()
    result = []
    reader = threading.Thread(target=lambda: result.append(engine.read_block(30000)))
    reader.start()
    time.sleep(0.02)
    began = time.monotonic()
    engine.stop()
    reader.join(timeout=2)
    assert not reader.is_alive()
    assert result == [None]
    assert time.monotonic() - began < 2


def test_error_propagation_and_restart(engine):
    engine.configure(config(simulated_fail_after=2))
    engine.start()
    wait_until(lambda: not engine.stats().running)
    assert engine.stats().errors == 1
    assert "Simulated" in engine.stats().last_error
    while True:
        try:
            block = engine.read_block(0)
        except RuntimeError as exc:
            assert "Simulated" in str(exc)
            break
        assert block is not None
        del block
    engine.stop()
    engine.configure(config())
    engine.start()
    block = engine.read_block(1000)
    assert block is not None


def test_request_timeout(engine):
    engine.configure(config(simulated_interval_ms=10000, timeout_ms=30))
    engine.start()
    with pytest.raises(RuntimeError, match="timed out"):
        engine.read_block(3000)
    engine.stop()
    assert engine.stats().errors == 1


@pytest.mark.parametrize("field,value", [
    ("block_size", 0), ("block_size", 4097), ("block_size", 17 * 1024 * 1024),
    ("queue_depth", 0), ("queue_depth", 65), ("queue_capacity", 0),
    ("pool_extra", 0), ("timeout_ms", 0),
])
def test_validation(engine, field, value):
    with pytest.raises(ValueError):
        engine.configure(config(**{field: value}))


def test_memory_budget(engine):
    with pytest.raises(ValueError, match="512 MiB"):
        engine.configure(config(block_size=16 * 1024 * 1024, queue_capacity=64))


def test_lifecycle(engine):
    engine.stop()  # idempotent
    engine.start()
    with pytest.raises(RuntimeError):
        engine.configure(config())
    with pytest.raises(RuntimeError):
        engine.start()
    engine.stop()
    engine.start()
    engine.stop()
    engine.close()
    engine.close()
    engine.open(simulated=True)
    engine.configure(config())
    engine.start()
    engine.stop()


def test_close_during_read_and_concurrent_stats(engine):
    engine.configure(config(simulated_interval_ms=10000, timeout_ms=20000))
    engine.start()
    reads = []
    reader = threading.Thread(target=lambda: reads.append(engine.read_block(30000)))
    reader.start()
    for _ in range(100):
        assert engine.stats().opened
    engine.close()
    reader.join(2)
    assert reads == [None]
    assert not engine.stats().opened
