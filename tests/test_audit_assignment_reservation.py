"""Fresh confirmed assignment and restart recovery share one reservation boundary."""
from pathlib import Path
from types import SimpleNamespace
import copy,json,sys,tempfile,threading,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/engine/runtime_host'),str(ROOT/'src/protocol')]
from horizon_network_audits import NetworkAudits
from horizon_assigned_audit_recovery import AssignedAuditRecovery
from horizon_network_lane import NetworkLane
CHAIN='a'*64;ANCHOR='b'*64;REQUEST='c'*64;ACTOR='0014'+'d'*40
RANGES=[dict(index=i,start=str(1000+i*100),end=str(1100+i*100))for i in range(5)]
REPORT=dict(request_id=REQUEST,ticket='e'*64,report_root='f'*64,miner_script='0014'+'1'*40)
def row():return dict(request_id=REQUEST,jobs=[dict(slot=1,attempt=0,actor=ACTOR,state='assigned',expired=False,actor_policy_accepted=True,ranges=copy.deepcopy(RANGES))])
class Lane(NetworkLane):
    def __init__(self):super().__init__();self.resume_attempt=threading.Event()
    def acquire(self):
        if threading.current_thread().name=='resume-lane':self.resume_attempt.set()
        return super().acquire()
class Rpc:
    def __init__(self):self.assigned=False;self.reports_entered=threading.Event();self.release_reports=threading.Event();self.recovery_entered=threading.Event();self.assign_count=0
    def call(self,method,params=None,wallet=False):
        params=params or []
        if method=='getpgnauditevent':
            if self.assigned:return dict(available=False,unavailable_reason='no_eligible_independent_work',genesis=CHAIN,parent=ANCHOR)
            return dict(available=True,challenge='fixture',envelope='fixture',request_id=REQUEST,slot=1,attempt=0,ranges=copy.deepcopy(RANGES))
        if method=='signmessage':return 'fixture-signature'
        if method=='getpgnreports':
            if threading.current_thread().name=='new-lane':
                self.reports_entered.set()
                if not self.release_reports.wait(4):raise AssertionError('fixture report release missing')
            return [dict(REPORT)]
        if method=='validateaddress':return dict(isvalid=True,scriptPubKey=ACTOR)
        if method=='getpgnworkview':
            self.recovery_entered.set()
            return dict(schema='PGN_WORK_VIEW_V1',kind='audits',offset=0,limit=100,state=dict(genesis=CHAIN,height=233,bestblockhash=ANCHOR),reports=[dict(REPORT)],audits=[row()])
        if method=='getbestblockhash':return ANCHOR
        if method=='getpgnauditstate':return [row()]
        if method=='getpgnscientificwork':return dict(target_numerator='32',target_denominator='1')
        raise AssertionError(method)
class ReservationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name);self.rpc=Rpc();self.lock=Lane();self.running=threading.Event()
        args=SimpleNamespace(genesis=CHAIN,piece_limit=100)
        owner=SimpleNamespace(root=self.root,rpc=self.rpc,args=args,network_lock=self.lock,address=lambda:'owned-fixture')
        self.recovery=AssignedAuditRecovery(owner,'audit-lanes');self.services=[]
        def ledger(envelopes):self.rpc.assigned=True;self.rpc.assign_count+=1;return dict(block_hash=ANCHOR)
        for i in range(2):
            folder=self.root/'audit-lanes'/str(i);folder.mkdir(parents=True)
            worker=SimpleNamespace(root=folder,rpc=self.rpc,args=args,network_lock=self.lock,address=lambda:'owned-fixture',source=SimpleNamespace(),running=self.running,ledger=ledger,recover_assigned_audit=self.recovery.claim)
            self.services.append(NetworkAudits(worker))
    def journals(self):return [json.loads((s.folder/'current.json').read_bytes())for s in self.services if(s.folder/'current.json').is_file()]
    def test_confirmed_assignment_is_reserved_before_other_lane_recovers(self):
        errors=[];outcomes={}
        def run(i):
            try:outcomes[i]=self.services[i].process_one()
            except BaseException as error:errors.append(error)
        first=threading.Thread(target=run,args=(0,),name='new-lane');second=threading.Thread(target=run,args=(1,),name='resume-lane')
        first.start()
        try:
            self.assertTrue(self.rpc.reports_entered.wait(2));second.start();self.assertTrue(self.lock.resume_attempt.wait(2))
            # The old implementation lets recovery consume the confirmed job
            # while its new assignment is still waiting for immutable metadata.
            self.rpc.recovery_entered.wait(.3)
        finally:
            self.rpc.release_reports.set();first.join(4)
            if second.ident is not None:second.join(4)
        self.assertFalse(first.is_alive());self.assertFalse(second.is_alive());self.assertEqual(errors,[])
        self.assertEqual(len(self.journals()),1,'one canonical slot was reserved by both physical lanes')
        self.assertEqual(self.rpc.assign_count,1);self.assertEqual(outcomes,{0:'paused',1:'none'})
    def test_restart_recovers_same_identity_once_and_keeps_pause(self):
        self.rpc.assigned=True;self.rpc.release_reports.set()
        self.assertEqual(self.services[0].process_one(),'paused')
        before=(self.services[0].folder/'current.json').read_bytes()
        fresh=NetworkAudits(self.services[0].worker)
        self.assertEqual(fresh.process_one(),'paused');self.assertEqual(self.services[1].process_one(),'none')
        self.assertEqual((fresh.folder/'current.json').read_bytes(),before);self.assertEqual(len(self.journals()),1)
        self.assertEqual(self.rpc.assign_count,0);self.assertFalse(self.running.is_set())
if __name__=='__main__':unittest.main()
