"""Coarse-grained acquisition API. DataBlock exports a read-only uint8 buffer."""
from ._native import (
    AcquisitionConfig, AcquisitionEngine, AcquisitionStats, DataBlock,
    DeviceInfo, list_devices,
)

__all__ = [
    "AcquisitionConfig", "AcquisitionEngine", "AcquisitionStats", "DataBlock",
    "DeviceInfo", "list_devices",
]
