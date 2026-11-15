"""Run the same audit-only rejection checks used by the frozen host."""
from pathlib import Path
import sys,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/engine/runtime_host'),str(ROOT/'src/protocol')]
from horizon_audit_batch_selftest import BatchTests
if __name__=='__main__':unittest.main()
