"""CORTEX — motor de pulsos con lexico emergente. Sin backprop, sin numpy."""

from .brain import Brain, Params, BucketQueue
from .assembly import Assembly, AssemblyDetector, Readout

__all__ = ["Brain", "Params", "BucketQueue", "Assembly", "AssemblyDetector", "Readout"]
__version__ = "0.1.0"
