"""Bounded activation delay tests against the actual shared lifecycle."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "src/update")]
import horizon_cohort_lifecycle as updater

class ControlledClock:
    now = 0
    def monotonic(self):
        return self.now
    def sleep(self, elapsed):
        self.now += elapsed

class StartupWarmupTest(unittest.TestCase):
    def launch(self, *, ready_at=None, expected_core=True, status=None, expires_at=None):
        clock = ControlledClock()
        data = Path("fixture/wallet")
        owner = SimpleNamespace(exited=lambda: expires_at is not None and clock.now >= expires_at)
        own_app = SimpleNamespace(pid=42)
        events = []
        def inspect(child, action):
            self.assertIs(child, owner)
            self.assertEqual(action, "--inspect-owned")
            return {
                "pgn_update_genesis": ("a" if expected_core else "b") * 64,
                "pgn_update_data_root": data.as_posix(),
                "pgn_core_link_state": "ready" if ready_at is not None and clock.now >= ready_at else "rpc_unavailable",
                "pgn_core_link_detail": status(clock.now) if status else "Core RPC read failed (4): Operation timed out",
            }
        state = SimpleNamespace(
            ui_relative="PGN-Horizon.exe", explicit_root=False, minimized=False, data=data, had_core=True,
            store=SimpleNamespace(context={"network": "testnet", "genesis_hash": "a" * 64}),
            launch_environment=dict, launch_options=dict,
            ipc=inspect, own_process=lambda *args: owner, record=events.append,
        )
        files = {"pgn-horizon.exe": {"sha256": "0" * 64}}
        with patch.object(updater, "time", clock), patch.object(updater, "inventory", return_value=(None, files, None, None)), \
             patch.object(updater.subprocess, "Popen", return_value=own_app):
            try:
                result = updater.NativeCohortLifecycle.launch(state, Path("fixture/code"))
            except RuntimeError:
                result = False
        return result, clock.now, events

    def test_real_core_warmup_survives_original_cutoff(self):
        condition = lambda now: "getblockchaininfo: Core RPC error (-28): Verifying blocks..."
        self.assertEqual(self.launch(ready_at=61, status=condition), (True, 61, ["verifying_startup", "healthy"]))
        self.assertEqual(self.launch(ready_at=61, expected_core=False, status=condition)[:2], (False, 60))
        self.assertEqual(self.launch(ready_at=61)[:2], (False, 60))

    def test_remaining_failures_respect_their_bounds(self):
        condition = lambda now: "getblockchaininfo: Core RPC error (-28): Verifying blocks..."
        cycling = lambda now: f"Core RPC error (-28): distinct init step {int(now) // 35}"
        self.assertEqual(self.launch(status=condition)[:2], (False, 120))
        self.assertEqual(self.launch(status=cycling)[:2], (False, 300))
        self.assertEqual(self.launch(status=condition, expires_at=17)[:2], (False, 17))

if __name__ == "__main__":
    unittest.main()
