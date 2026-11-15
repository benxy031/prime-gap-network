"""Closed frozen-host recovery contract checks: no Core, socket or native worker."""
from pathlib import Path
from types import SimpleNamespace
import copy,json,tempfile,threading
from horizon_cpu_source import CpuOnlyNormalSource
from horizon_cpu_service_work import service_request,SCHEMA
from horizon_assigned_audit_recovery import AssignedAuditRecovery
from horizon_network_audits import partition_ticket
from horizon_mining_flow import MiningFlow

def require(value,message):
    if not value:raise RuntimeError(message)

def rejected(call):
    try:call()
    except ValueError:return True
    raise RuntimeError('Invalid CPU service context was accepted')

def _run(version):
    chain='a'*64;anchor='b'*64;request='c'*64;actor='0014'+'d'*40;start=151219600000000000000
    ranges=[dict(index=i,start=str(start+i*2000000000000),end=str(start+(i+1)*2000000000000))for i in(0,9,20,40,99)]
    audit=dict(schema='PGN_HORIZON_ASSIGNED_AUDIT_V1',request_id=request,ticket='e'*64,report_root='f'*64,slot=1,attempt=0,ranges=ranges,counts=[],target_merit_q16=1802240)
    class Core:
        def __init__(self):self.tip=anchor;self.owned=True;self.job=dict(slot=1,attempt=0,actor=actor,state='assigned',expired=False,actor_policy_accepted=True,issued=233,issued_time=1000,deadline_time=87400,deadline_clock='median_chain_time',ranges=ranges)
        def status(self):return dict(schema='PGN_NATIVE_WORK_DUTY_STATUS_V1',genesis=chain,bestblockhash=self.tip,enforced=True,may_request=False,credits=[])
        def call(self,method,args=None,wallet=False):
            if method=='getaddressinfo':return dict(ismine=self.owned,scriptPubKey=actor)
            if method=='getpgnworkdutystatus':return self.status()
            if method=='getbestblockhash':return self.tip
            if method=='getpgnworkview':return dict(schema='PGN_WORK_VIEW_V1',kind='audits',state=dict(genesis=chain,bestblockhash=self.tip,height=440,median_time_past=2000),reports=[dict(request_id=request,ticket='e'*64,report_root='f'*64)],audits=[dict(request_id=request,jobs=[copy.deepcopy(self.job)])])
            raise RuntimeError('Self-test attempted an external operation: '+method)
    checks={}
    with tempfile.TemporaryDirectory(prefix='pgn-host-contract-')as temporary:
        root=Path(temporary);core=Core();source=CpuOnlyNormalSource(root)
        worker=SimpleNamespace(root=root,rpc=core,args=SimpleNamespace(genesis=chain),source=source,address=lambda:'owned-fixture',network_lock=threading.RLock(),device_lease=object(),audits=SimpleNamespace())
        ticket=partition_ticket(audit,ranges[0]);context=dict(role='audit',work_cycle=None,audit_position=0,audit_request_id=request,audit_slot=1,audit_attempt=0,presentation={});job=SimpleNamespace(ticket=ticket,context=context,view_sequence=1)
        flow=object.__new__(MiningFlow);flow.worker=worker;flow.extra_audit_owner=lambda value:None;flow.unbound_service_lock=threading.RLock();flow.unbound_service_completed=set();flow.unbound_service_audits=set();flow.unbound_service_seeded=True;flow.prefetch=SimpleNamespace(service_observation=core.status)
        try:
            require(flow.may_start_work(job),'Native service was not admitted');flow.activate(job);grant=copy.deepcopy(source.grant)
            require(grant['schema']==SCHEMA and 'normal_ticket'not in grant and 'work_cycle'not in grant,'Service/search contracts were conflated');checks['audit_before_ticket']=True
            require(source.run_ticket(ticket,10000)==dict(state='PAUSED'),'OFF was not conserved');checks['off_preserved']=True
            for key,value in [('actor','0014'+'2'*40),('slot',0),('attempt',1),('expired',True),('deadline_time',1999)]:
                old=core.job[key];core.job[key]=value;rejected(lambda:service_request(worker,ticket,context));core.job[key]=old;checks['reject_'+key]=True
            core.owned=False;rejected(lambda:service_request(worker,ticket,context));core.owned=True;checks['reject_unowned_key']=True
            source.close();source=None;core.tip='8'*64
            source=CpuOnlyNormalSource(root);worker.source=source;require(flow.may_start_work(job),'Restart service was not admitted');flow.activate(job)
            require(source.grant==grant,'Moving tip changed immutable work identity');require(source.run_ticket(ticket,10000)==dict(state='PAUSED'),'Restart enabled CPU');checks['restart_identity_and_off']=True
            recovery=AssignedAuditRecovery(worker,'audit-lanes');files=[]
            for i in range(2):
                p=root/'audit-lanes'/str(i)/'audits/current.json';p.parent.mkdir(parents=True);p.write_text(json.dumps(audit));files.append(p)
            raw=files[1].read_bytes();require(recovery.reconcile_duplicates(),'Duplicate was not recovered');require(not recovery.reconcile_duplicates(),'Recovery was not idempotent')
            copies=list((files[1].parent/'retained-assignments').glob('*.json'));require(len(copies)==1 and copies[0].read_bytes()==raw,'Original duplicate proof was not conserved');checks['duplicate_restart_idempotent']=True
            files[1].write_text(json.dumps(dict(audit,counts=[2])));files[0].write_text(json.dumps(dict(audit,counts=[1])));before=[p.read_bytes()for p in files]
            rejected(recovery.reconcile_duplicates);require([p.read_bytes()for p in files]==before,'Conflicting counts were changed');checks['conflicts_preserved']=True
        finally:
            if source is not None:source.close()
    return dict(schema='PGN_ENGINE_HOST_SELF_TEST_V1',PASS=True,version=version,frozen=bool(getattr(__import__('sys'),'frozen',False)),checks=checks,network_requested=False,installation_performed=False,signing_performed=False,native_worker_started=False,scientific_promotion=False)


def run(version):
    from unittest.mock import patch
    def blocked(*args,**kwargs):raise RuntimeError('Self-test attempted network or child-process creation')
    with patch('socket.socket',blocked),patch('socket.create_connection',blocked),patch('subprocess.Popen',blocked):
        return _run(version)
