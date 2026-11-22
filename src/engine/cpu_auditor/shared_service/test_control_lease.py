import pathlib,sys,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from service import ControlLease

class ControlLeaseTests(unittest.TestCase):
 def setUp(self):self.now=0.;self.lease=ControlLease(45,5,lambda:self.now)
 def test_idle_pipe_cannot_hold_workspace_forever(self):
  self.now=44.999;self.assertFalse(self.lease.close_due())
  self.now=45;self.assertTrue(self.lease.close_due());self.assertFalse(self.lease.drain_expired())
  self.now=49.999;self.assertFalse(self.lease.drain_expired())
  self.now=50;self.assertTrue(self.lease.drain_expired())
 def test_live_control_renews_without_additional_messages(self):
  for i in range(1,100):
   self.now=i*44;self.assertFalse(self.lease.close_due());self.lease.observe()
  self.now+=45;self.assertTrue(self.lease.close_due())
 def test_eof_closes_immediately_and_late_command_cannot_revive_it(self):
  self.now=1;self.assertTrue(self.lease.close_due(eof=True))
  self.now=2;self.lease.observe();self.assertEqual(self.lease.closing_since,1)
  self.now=6;self.assertTrue(self.lease.close_due());self.assertTrue(self.lease.drain_expired())
 def test_invalid_limits_rejected(self):
  for timeout,grace in[(0,5),(-1,5),(45,-1)]:
   with self.assertRaises(ValueError):ControlLease(timeout,grace)

if __name__=='__main__':unittest.main()
