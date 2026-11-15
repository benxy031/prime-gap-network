from pathlib import Path
import sys,unittest
root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'tests'),str(root/'src/update'),str(root/'src/protocol'),str(root/'src/engine/runtime_host')]
names=['test_cycle_execution','test_mining_flow_cycle','test_audit_cycle_reassignment','test_audit_cycle_publication','test_catchup_budget_flow','test_physical_binding_recovery','test_gpu_service_handoff','test_late_cycle_completion']
result=unittest.TextTestRunner(verbosity=1).run(unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromName(n)for n in names))
raise SystemExit(not result.wasSuccessful())
