"""Locate the Gold evidence builder that the publication path must execute.

The Linux runtime packager places the helper as `pgn-native-gap-evidence` at the
runtime root, while the Windows bundle and the component manifest use
`bin/pgn-gap-evidence`. A worker that only looked at `bin/pgn-gap-evidence`
therefore never found it on Linux, and every Gold publication failed inside a
maintenance job - 373 ticket folders with an empty gold-publications/ folder and
zero published discoveries. Probing the known layouts fixes that without
changing any of them, and an explicit --proof-builder still wins.

Nothing here runs a binary; it only answers "where is it" and says exactly what
it looked for when the answer is nowhere.
"""
import os
from pathlib import Path


class GoldBuilderMissing(RuntimeError):
    retryable = False


def names(nt=None):
    nt = os.name == "nt" if nt is None else nt
    return ("pgn-gap-evidence.exe", "pgn-native-gap-evidence.exe") if nt else (
        "pgn-gap-evidence", "pgn-native-gap-evidence")


def candidates(bundle, nt=None):
    """Every location a shipped runtime is known to use, in preference order."""
    bundle = Path(bundle)
    roots = (bundle / "bin", bundle, bundle.parent, bundle.parent / "bin")
    return [root / name for root in roots for name in names(nt)]


def usable(path):
    return Path(path).is_file() and os.access(path, os.X_OK)


def resolve(bundle, explicit=None, nt=None):
    """The builder to execute, or GoldBuilderMissing naming every path tried."""
    if explicit is not None:
        path = Path(explicit)
        if not usable(path):
            raise GoldBuilderMissing("Gold evidence builder is not an executable file: " + str(path))
        return path
    tried = candidates(bundle, nt)
    for path in tried:
        if usable(path):
            return path
    raise GoldBuilderMissing("Gold evidence builder missing; tried " +
                             ", ".join(str(path) for path in tried))
