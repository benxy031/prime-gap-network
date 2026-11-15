from pathlib import Path
import sys,tempfile,unittest
HERE=Path(__file__).resolve().parent
Q=HERE.parent
sys.path[:0]=[str(Q/'src/protocol'),str(Q/'src/engine/runtime_host')]
from horizon_cycle_execution import CycleExecution

class LateCycleCompletion(unittest.TestCase):
    def test_retired_callbacks_cannot_refill_live_view(self):
        with tempfile.TemporaryDirectory(prefix='owned-cycle-test-',dir=HERE)as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to(HERE.resolve()))
            journal=CycleExecution(folder,'a'*64)
            retired=[]
            for i in range(1,101):
                cycle=f'{i:064x}';hot={f'{n:064x}'for n in range(max(1,i-3),i+1)}
                journal.reconcile_extra_completion(cycle,f'{1000+i:064x}',f'{2000+i:064x}',hot)
                for old in retired[-8:]:
                    self.assertEqual(journal.reconcile_extra_completion(old,f'{5000+i:064x}','e'*64,hot),old in hot)
                self.assertLessEqual(len(journal.extras),4)
                self.assertTrue(set(journal.extras)<=hot)
                self.assertIn(f'{1000+i:064x}',journal.completed_extras(cycle))
                retired.append(cycle)
                # Actual disk reopening must keep the same exact current receipts.
                reopened=CycleExecution(folder,'a'*64)
                self.assertEqual(reopened.extras,journal.extras)

    def test_duplicate_and_malformed_callback_preserve_receipt(self):
        with tempfile.TemporaryDirectory(prefix='owned-cycle-test-',dir=HERE)as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to(HERE.resolve()))
            journal=CycleExecution(folder,'a'*64);cycle='b'*64;audit='c'*64;block='d'*64
            journal.reconcile_extra_completion(cycle,audit,block,{cycle})
            before=journal.extra_path.read_bytes()
            journal.reconcile_extra_completion(cycle,audit,block,{cycle})
            self.assertEqual(before,journal.extra_path.read_bytes())
            with self.assertRaises(ValueError):journal.reconcile_extra_completion(cycle,'bad',block,{cycle})
            self.assertEqual(before,journal.extra_path.read_bytes())
            with self.assertRaises(ValueError):journal.reconcile_extra_completion(cycle,audit,block,{f'{i:064x}'for i in range(1,6)})
            self.assertEqual(before,journal.extra_path.read_bytes())

if __name__=='__main__':unittest.main()
