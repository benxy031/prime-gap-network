from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,'-I',str(root/'tools/check_lifecycle226.py')],check=True)
for name in ['test_audit_reveal_batch', 'test_network_audit_publication', 'test_network_audit_lifecycle', 'test_audit_ack_reorganization', 'test_atomic_audit_snapshot', 'test_audit_answer_fairness', 'test_audit_publication_isolation', 'test_retained_owner_publication', 'test_audit_seal_recovery', 'test_audit_seal_flow', 'test_coordination_receipt', 'test_coordination_rebase', 'test_coordination_parent_change']:
    if name=='test_audit_answer_fairness' and '--full' not in sys.argv:
        print('5000-answer disk stress reserved for the disposable full CI run',flush=True)
        continue
    print('Checking '+name,flush=True)
    subprocess.run([sys.executable,'-I',str(root/'tests'/(name+'.py'))],check=True)
