"""Own-Core CPU audit service before ticket: identity, lease, restart and OFF."""
from pathlib import Path
from types import SimpleNamespace
import copy,hashlib,json,sqlite3,sys,tempfile,threading,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'src/engine/runtime_host'),str(ROOT/'src/protocol'),str(ROOT/'src/engine/cpu_auditor/shared_service')]
from horizon_cpu_source import CpuOnlyNormalSource,CpuScientificSourcePending
from horizon_cpu_service_work import service_request,SCHEMA
from horizon_mining_flow import MiningFlow
from horizon_network_audits import partition_ticket
from horizon_assigned_audit_recovery import AssignedAuditRecovery
from report_commitment import audit_commitment
from test_cpu_only_source import Endpoint
import cycle_work
CHAIN='a'*64;ANCHOR='b'*64;REQUEST='c'*64;ACTOR='0014'+'d'*40;LO=151219600000000000000
RANGES=[dict(index=i,start=str(LO+i*2000000000000),end=str(LO+(i+1)*2000000000000))for i in (0,9,20,40,99)]
def audit():return dict(schema='PGN_HORIZON_ASSIGNED_AUDIT_V1',request_id=REQUEST,ticket='e'*64,report_root='f'*64,slot=1,attempt=0,ranges=copy.deepcopy(RANGES),counts=[],target_merit_q16=1802240)
class Core:
    def __init__(self):
        self.anchor=ANCHOR;self.tip=ANCHOR;self.owned=True;self.permission=False;self.calls=[]
        self.job=dict(slot=1,attempt=0,actor=ACTOR,state='assigned',expired=False,actor_policy_accepted=True,issued=233,issued_time=1000,deadline_time=87400,deadline_clock='median_chain_time',ranges=copy.deepcopy(RANGES))
    def status(self):return dict(schema='PGN_NATIVE_WORK_DUTY_STATUS_V1',enforced=True,may_request=self.permission,genesis=CHAIN,bestblockhash=self.anchor,credits=[])
    def call(self,method,args=None,wallet=False):
        self.calls.append(method)
        if method=='getaddressinfo':return dict(ismine=self.owned,solvable=self.owned,scriptPubKey=ACTOR)
        if method=='getpgnworkdutystatus':return self.status()
        if method=='getpgnworkview':return dict(schema='PGN_WORK_VIEW_V1',kind='audits',state=dict(genesis=CHAIN,bestblockhash=self.anchor,height=440,median_time_past=2000),reports=[dict(request_id=REQUEST,ticket='e'*64,report_root='f'*64)],audits=[dict(request_id=REQUEST,jobs=[copy.deepcopy(self.job)])])
        if method=='getbestblockhash':return self.tip
        raise AssertionError(method)
class CompleteEndpoint(Endpoint):
    def __init__(self):super().__init__();self.executions=[]
    def status(self):return dict(super().status(),native_audit_service_schema=SCHEMA,complete_record_delivery='PGN_CPU_COMPLETE_RECORDS_V1')
    def call(self,action,**kw):
        if action=='execute':self.executions.append(copy.deepcopy(kw['request']))
        if action=='records':
            lo=int(self.request['start'])+1
            return dict(records=dict(schema='PGN_CPU_COMPLETE_RECORDS_V1',request_sha256=kw['request_sha256'],total_records='1',records=[dict(lower_prime=str(lo),upper_prime=str(lo+1000),gap=1000)],next_after=None))
        return super().call(action,**kw)
class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name);self.core=Core();self.source=None;self.endpoint=CompleteEndpoint();self.open()
    def tearDown(self):
        if self.source:self.source.close()
    def open(self):
        self.source=CpuOnlyNormalSource(self.root);self.source.clients['CPU/TEST/0']=self.endpoint;self.source.options['CPU/TEST/0']=dict(enabled=False,threads=1,affinity=[0])
        self.worker=SimpleNamespace(root=self.root,rpc=self.core,args=SimpleNamespace(genesis=CHAIN),source=self.source,address=lambda:'owned-fixture',network_lock=threading.RLock(),device_lease=object(),audits=SimpleNamespace())
        self.flow=object.__new__(MiningFlow);self.flow.worker=self.worker;self.flow.extra_audit_owner=lambda job:None;self.flow.unbound_service_lock=threading.RLock();self.flow.unbound_service_completed=set();self.flow.unbound_service_audits=set();self.flow.unbound_service_seeded=True;self.flow.prefetch=SimpleNamespace(service_observation=self.core.status)
        a=audit();self.ticket=partition_ticket(a,a['ranges'][0]);self.context=dict(role='audit',work_cycle=None,audit_position=0,audit_request_id=REQUEST,audit_slot=1,audit_attempt=0,presentation={});self.job=SimpleNamespace(ticket=self.ticket,context=self.context,view_sequence=1)
    def grant(self):
        self.assertTrue(self.flow.may_start_work(self.job));self.flow.activate(self.job);return self.source.grant
    def enable(self):self.source.options['CPU/TEST/0']['enabled']=True
    def test_pre_ticket_audit_is_typed_and_off_remains_off(self):
        grant=self.grant();self.assertEqual(grant['schema'],SCHEMA);self.assertNotIn('normal_ticket',grant);self.assertNotIn('work_cycle',grant)
        self.assertEqual(grant['actor'],ACTOR);self.assertEqual(grant['slot'],1);self.assertEqual(grant['attempt'],0)
        self.assertEqual(self.source.run_ticket(self.ticket,10000),dict(state='PAUSED'));self.assertEqual(self.endpoint.executions,[])
        self.assertFalse(self.source.options['CPU/TEST/0']['enabled']);self.assertFalse(any(x in self.core.calls for x in ('signmessage','getpgnauditevent','submitblock')))
    def test_compute_uses_exact_service_partition_without_publication_or_credit(self):
        self.grant();self.enable();result=self.source.run_ticket(self.ticket,10000)
        self.assertEqual(len(self.endpoint.executions),1);self.assertEqual(self.endpoint.executions[0]['ranges'],RANGES);self.assertEqual(len(cycle_work.partitions(self.endpoint.executions[0])),100)
        self.assertEqual(result['body']['positions'],'2000000000000');self.assertFalse(result['network_accepted']);self.assertTrue(result['scientific_finalization_pending'])
        with self.assertRaises(CpuScientificSourcePending):self.source.finalize_report({},result)
    def test_real_executor_checkpoint_restores_service_contract_with_off_conserved(self):
        from service import Service
        grant=self.grant();executor=Service.__new__(Service);executor.request=None;executor.enabled=False;executor.cp=self.root/'executor-checkpoint.json'
        executor.new_work(dict(request=grant),False)
        self.assertEqual(executor.request,grant);self.assertFalse(executor.enabled)
        restored=Service.__new__(Service);restored.cp=executor.cp;restored.enabled=False;restored.restore()
        self.assertEqual(restored.request,grant);self.assertEqual(restored.counts,[0]*100);self.assertFalse(restored.enabled);self.assertEqual(restored.state,'PAUSED_RECOVERED')

    def test_foreign_actor_wallet_slot_attempt_and_lease_are_rejected(self):
        changes=[('actor','0014'+'2'*40),('slot',0),('attempt',1),('expired',True),('state','committed'),('actor_policy_accepted',False),('deadline_clock','height'),('deadline_time',1999)]
        for key,value in changes:
            with self.subTest(key=key):
                old=self.core.job[key];self.core.job[key]=value
                with self.assertRaises(ValueError):service_request(self.worker,self.ticket,self.context)
                self.core.job[key]=old
        self.core.owned=False
        with self.assertRaises(ValueError):service_request(self.worker,self.ticket,self.context)
        self.assertEqual(self.endpoint.executions,[])
    def test_wrong_request_and_partition_bounds_are_rejected(self):
        for change in (dict(audit_request_id='9'*64),dict(audit_slot=0),dict(audit_attempt=1)):
            with self.assertRaises(ValueError):service_request(self.worker,self.ticket,dict(self.context,**change))
        with self.assertRaises(ValueError):service_request(self.worker,dict(self.ticket,end=str(int(self.ticket['end'])-100)),self.context)
    def test_existing_owned_reservation_can_finish_after_other_credits_arrive(self):
        self.flow.unbound_service_audits.add((REQUEST,1,0));self.core.permission=True
        self.assertEqual(self.grant()['request_id'],REQUEST);self.assertFalse(self.source.options['CPU/TEST/0']['enabled'])

    def test_tip_race_keeps_job_queued_without_enabling_cpu(self):
        self.core.tip='9'*64;self.assertFalse(self.flow.may_start_work(self.job));self.assertNotIn('cpu_service_grant',self.context);self.assertIsNone(self.source.grant)
        self.core.tip=self.core.anchor;self.assertTrue(self.flow.may_start_work(self.job));self.assertFalse(self.source.options['CPU/TEST/0']['enabled'])
    def test_restart_reuses_exact_delivery_across_tip_extension(self):
        grant=self.grant();self.enable();self.endpoint.lose_ack=True
        with self.assertRaises(OSError):self.source.run_ticket(self.ticket,10000)
        self.source.close();self.source=None;self.core.anchor='8'*64;self.core.tip=self.core.anchor;self.open();self.assertEqual(self.grant(),grant);self.enable()
        result=self.source.run_ticket(self.ticket,10000);again=self.source.run_ticket(self.ticket,10000)
        self.assertEqual(result['body'],again['body']);self.assertEqual(len(self.endpoint.executions),1);self.assertFalse(result['network_accepted'])
    def test_executor_requires_typed_slot_attempt_and_full_lease_partitions(self):
        request=self.grant();self.assertEqual(len(cycle_work.partitions(request)),100)
        changes=[lambda v:v.update(normal_ticket='1'*64),lambda v:v.update(expected_counts=[0]*5),lambda v:v.update(slot=0),lambda v:v.update(attempt=1),lambda v:v.update(actor='invalid'),lambda v:v.update(deadline_time=v['issued_time']),lambda v:v['ranges'][0].update(end=str(int(v['ranges'][0]['end'])-100))]
        for mutate in changes:
            v=copy.deepcopy(request);mutate(v)
            with self.assertRaises(ValueError):cycle_work.partitions(v)
    def test_old_executor_is_refused_without_replacing_retained_work(self):
        self.grant();self.enable();original=self.endpoint.status
        self.endpoint.status=lambda:{k:v for k,v in original().items()if k!='native_audit_service_schema'}
        with self.assertRaisesRegex(CpuScientificSourcePending,'Installed CPU executor'):self.source.run_ticket(self.ticket,10000)
        self.assertEqual(self.endpoint.executions,[])
class DuplicateTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name);self.core=Core();self.worker=SimpleNamespace(root=self.root,rpc=self.core,args=SimpleNamespace(genesis=CHAIN),address=lambda:'owned-fixture',network_lock=threading.RLock());self.recovery=AssignedAuditRecovery(self.worker,'audit-lanes');self.files=[]
        for i in range(2):
            p=self.root/'audit-lanes'/str(i)/'audits/current.json';p.parent.mkdir(parents=True);p.write_text(json.dumps(audit()));self.files.append(p)
    def test_existing_duplicate_recovery_is_idempotent_and_retains_original_bytes(self):
        originals=[p.read_bytes()for p in self.files];self.assertTrue(self.recovery.reconcile_duplicates());self.assertTrue(self.files[0].exists());self.assertFalse(self.files[1].exists());self.assertEqual(self.files[0].read_bytes(),originals[0])
        saved=self.files[1].parent/'retained-assignments'/(hashlib.sha256(originals[1]).hexdigest()+'.json');self.assertEqual(saved.read_bytes(),originals[1]);self.assertFalse(self.recovery.reconcile_duplicates())
        self.assertFalse(any(x in self.core.calls for x in ('signmessage','getpgnauditevent','submitblock')))
    def test_interrupted_duplicate_archive_is_reused_byte_for_byte(self):
        raw=self.files[1].read_bytes();archive=self.files[1].parent/'retained-assignments';archive.mkdir();saved=archive/(hashlib.sha256(raw).hexdigest()+'.json');saved.write_bytes(raw)
        self.assertTrue(self.recovery.reconcile_duplicates());self.assertEqual(saved.read_bytes(),raw);self.assertFalse(self.recovery.reconcile_duplicates())
    def test_catalogued_original_seal_is_not_replaced_by_empty_journal(self):
        name=REQUEST+'-1-0';database=self.files[1].parent/'answers.sqlite3';db=sqlite3.connect(database)
        try:db.execute('CREATE TABLE answers(name TEXT PRIMARY KEY,digest TEXT)');db.execute('INSERT INTO answers VALUES(?,?)',(name,'9'*64));db.commit()
        finally:db.close()
        before=[p.read_bytes()for p in self.files]
        with self.assertRaisesRegex(ValueError,'Registered duplicate audit seal'):self.recovery.reconcile_duplicates()
        self.assertEqual([p.read_bytes()for p in self.files],before)

    def test_more_complete_prefix_wins_without_recomputation_or_nonce(self):
        v=audit();v['counts']=[7];self.files[1].write_text(json.dumps(v));before=self.files[1].read_bytes();self.assertTrue(self.recovery.reconcile_duplicates());self.assertEqual(self.files[1].read_bytes(),before);self.assertFalse(self.files[0].exists())
    def test_conflicting_counts_seals_foreign_actor_and_cycles_preserve_both(self):
        mutations=[lambda a,b:(a.update(counts=[1]),b.update(counts=[2])),lambda a,b:b.update(report_root='9'*64),lambda a,b:(a.update(work_cycle='9'*64),b.update(work_cycle='9'*64))]
        for mutate in mutations:
            a,b=audit(),audit();mutate(a,b)
            for p,v in zip(self.files,(a,b)):p.write_text(json.dumps(v))
            before=[p.read_bytes()for p in self.files]
            with self.assertRaises(ValueError):self.recovery.reconcile_duplicates()
            self.assertEqual([p.read_bytes()for p in self.files],before)
        for p in self.files:p.write_text(json.dumps(audit()))
        self.core.owned=False;before=[p.read_bytes()for p in self.files]
        with self.assertRaises(ValueError):self.recovery.reconcile_duplicates()
        self.assertEqual([p.read_bytes()for p in self.files],before)
    def test_conflicting_valid_seals_are_not_chosen(self):
        for p,nonce in zip(self.files,('1'*64,'2'*64)):
            v=audit();v['counts']=[7]*5;v['answer']=audit_commitment(v['ticket'],0,[r['index']for r in v['ranges']],['7']*5,nonce);p.write_text(json.dumps(v))
        before=[p.read_bytes()for p in self.files]
        with self.assertRaisesRegex(ValueError,'Conflicting duplicate audit seals'):self.recovery.reconcile_duplicates()
        self.assertEqual([p.read_bytes()for p in self.files],before)
if __name__=='__main__':unittest.main()
