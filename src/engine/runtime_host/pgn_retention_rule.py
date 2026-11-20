"""Use the retained-merit arithmetic shared with the CPU controller."""
from pathlib import Path
import sys

# Source execution has separate module roots. The frozen host includes the
# canonical module through its build manifest, without a source-tree dependency.
_source_directory = None if getattr(sys, "frozen", False) else str(
    Path(__file__).resolve().parents[1] / "cpu_auditor" / "shared_service"
)
if _source_directory is not None:
    sys.path.insert(0, _source_directory)
try:
    from merit_policy import (
        DOMAIN_END, DOMAIN_START, MINIMUM_MERIT, LogicalPiece,
        minimum_gap, next_threshold_integer, next_piece, pieces,
    )
finally:
    if _source_directory is not None:
        sys.path.remove(_source_directory)
