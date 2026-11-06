"""Run with --hardware only after selecting the correct device/endpoint."""
import argparse
import time
import numpy as np
from fx3_cyapi import AcquisitionConfig, AcquisitionEngine

parser = argparse.ArgumentParser()
parser.add_argument("--hardware", action="store_true")
parser.add_argument("--index", type=int, default=0)
parser.add_argument("--endpoint", type=lambda x: int(x, 0), default=0x81)
parser.add_argument("--seconds", type=float, default=3)
args = parser.parse_args()
with AcquisitionEngine() as engine:
    engine.open(index=args.index, endpoint=args.endpoint, simulated=not args.hardware)
    config = AcquisitionConfig()
    engine.configure(config)
    engine.start()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        block = engine.read_block(timeout_ms=100)
        if block is not None:
            values = np.frombuffer(block, dtype=np.uint8)
            print(f"block={block.sequence} bytes={block.size} preview={values[:8].tolist()}")
            del values, block
    engine.stop()
    s = engine.stats()
    print(f"received={s.received_bytes} dropped={s.dropped_blocks} errors={s.errors}")
