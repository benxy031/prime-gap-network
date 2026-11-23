"""Bounded active-index discovery checks against the real recovery class."""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from collections import Counter
import copy
import json
import sys
import tempfile
import threading
import unittest

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
sys.path[:0] = [str(SOURCE/'src/engine/runtime_host'), str(SOURCE/'src/protocol')]
import horizon_assigned_audit_recovery as module
from horizon_assigned_audit_recovery import AssignedAuditRecovery, LegacyAuditSignerUnresolved
from horizon_core_rpc import CoreRpcError

CHAIN='a'*64; ANCHOR='b'*64; ACTOR='0014'+'c'*40; OTHER='0014'+'1'*40
OBSERVATIONS={}

def report(n):
    return dict(request_id=f'{n:064x}', ticket='d'*64, report_root='e'*64, miner_script=OTHER)

def job(actor=ACTOR, slot=0, attempt=0):
    return dict(slot=slot, attempt=attempt, state='assigned', actor=actor, expired=False,
                actor_policy_accepted=True,
                ranges=[dict(index=i,start=str(1000+100*i),end=str(1100+100*i)) for i in range(5)])

def activity(entries):
    rows=[dict(request_id=report(n)['request_id'],jobs=jobs) for n,jobs in entries]
    active=sum(j['state']=='committed' or j['state']=='assigned' and not j['expired'] for r in rows for j in r['jobs'])
    return dict(schema='PGN_AUDIT_ACTIVITY_V1',scope='global_counts_with_bounded_public_job_rows',
                after_height=0,rebase=True,truncated=False,
                state=dict(genesis=CHAIN,bestblockhash=ANCHOR,height=1000,median_time_past=100),
                totals=dict(active_reports=len(rows),active_jobs=active,recent_reports=0,
                            passed_audits_since_height=0,selected_active_reports=len(rows),selected_recent_reports=0),
                audits=rows)

def exact(n,jobs,anchor=ANCHOR):
    return dict(schema='PGN_WORK_VIEW_V1',kind='audits',offset=0,limit=1,request_id=report(n)['request_id'],
                state=dict(genesis=CHAIN,bestblockhash=anchor,height=1000),
                reports=[report(n)],audits=[dict(request_id=report(n)['request_id'],jobs=jobs)])

def history(offset):
    # Deliberately full nonterminal page: fallback is observable and cannot
    # silently pass a test by establishing absence from an empty history.
    return dict(schema='PGN_WORK_VIEW_V1',kind='audits',offset=offset,limit=100,
                state=dict(genesis=CHAIN,bestblockhash=ANCHOR,height=1000),
                reports=[report(10000+i) for i in range(100)],
                audits=[dict(request_id=report(10000+i)['request_id'],jobs=[]) for i in range(100)])

class Rpc:
    def __init__(self):
        self.calls=[];self.actor=ACTOR;self.tip=ANCHOR
        self.active=activity([(1,[job()])]);self.exacts={report(1)['request_id']:exact(1,[job()])}
        self.unsupported=False;self.flip_after_active=False;self.history_jobs={}
    def call(self,method,args=None,**kwargs):
        self.calls.append((method,args))
        if method=='validateaddress':return dict(isvalid=True,scriptPubKey=self.actor)
        if method=='getbestblockhash':return self.tip
        if method=='getpgnauditactivity':
            if self.unsupported:raise CoreRpcError(-32601,'Method not found')
            value=copy.deepcopy(self.active)
            if self.flip_after_active:self.tip='9'*64
            return value
        if method=='getpgnworkview':
            if len(args)==4:return copy.deepcopy(self.exacts[args[3]])
            value=history(args[1]);value['state']['bestblockhash']=self.tip
            for row in value['audits']:
                row['jobs']=copy.deepcopy(self.history_jobs.get(row['request_id'],[]))
            return value
        if method=='decodescript':return dict(address='own')
        if method=='getaddressinfo':return dict(ismine=True,iswatchonly=False,scriptPubKey=self.actor)
        if method=='getwalletinfo':return dict(private_keys_enabled=True)
        raise AssertionError((method,args))

class ActiveDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='pgn-active-audit-');self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.rpc=Rpc()
        self.worker=SimpleNamespace(root=self.root,rpc=self.rpc,args=SimpleNamespace(genesis=CHAIN),
                                    network_lock=threading.RLock(),address=lambda:'own')
        self.recovery=AssignedAuditRecovery(self.worker)
        self.folders=[self.root/'service-audit-lanes'/str(i)/'audits' for i in range(2)]
        for f in self.folders:f.mkdir(parents=True)
    def history_calls(self):
        return [args for name,args in self.rpc.calls if name=='getpgnworkview' and len(args)==3]
    def reserve(self,n=1,slot=0):
        value=dict(request_id=report(n)['request_id'],ticket=report(n)['ticket'],report_root=report(n)['report_root'],
                   actor=ACTOR,slot=slot,attempt=0,ranges=job(slot=slot)['ranges'],counts=[])
        (self.folders[1]/'current.json').write_text(json.dumps(value),encoding='utf8')
        return value
    def assert_fallback(self):
        self.assertEqual(self.recovery.claim(self.folders[0]),(None,False))
        self.assertEqual(len(self.history_calls()),1)
        self.assertFalse((self.folders[0]/'current.json').exists())
    def test_recovers_exact_current_assignment_without_history_pages(self):
        audit,complete=self.recovery.claim(self.folders[0],verify_signer=True)
        self.assertIsNotNone(audit)
        self.assertEqual(audit['request_id'],report(1)['request_id']);self.assertFalse(complete)
        self.assertEqual(audit['actor'],ACTOR);self.assertEqual(audit['ranges'],job()['ranges'])
        self.assertEqual(self.history_calls(),[])
        self.assertEqual(json.loads((self.folders[0]/'current.json').read_text())['report_root'],report(1)['report_root'])
    def test_802_only_exact_reserved_sibling_means_no_other_recoverable_work(self):
        self.rpc.active=activity([(1,[job(slot=1)])]);self.rpc.exacts[report(1)['request_id']]=exact(1,[job(slot=1)])
        before=self.reserve(slot=1);self.recovery.cursor.write_text(json.dumps(dict(genesis=CHAIN,actor=ACTOR,offset=2100)))
        old=self.recovery.cursor.read_bytes()
        self.assertEqual(self.recovery.claim(self.folders[0]),(None,True))
        self.assertEqual(self.history_calls(),[]);self.assertEqual(self.recovery.cursor.read_bytes(),old)
        self.assertEqual(json.loads((self.folders[1]/'current.json').read_text()),before)
        OBSERVATIONS['802']=dict(counts=dict(Counter(x[0]for x in self.rpc.calls)),history_pages=0,cursor_preserved=True)
    def test_empty_complete_active_view_reuses_existing_anchor_cache(self):
        self.rpc.active=activity([])
        self.assertEqual(self.recovery.claim(self.folders[0]),(None,True))
        self.assertEqual(self.recovery.claim(self.folders[1]),(None,True))
        self.assertEqual(sum(m=='getpgnauditactivity'for m,_ in self.rpc.calls),1)
        self.assertEqual(self.history_calls(),[])
    def test_other_actor_is_not_recovered(self):
        self.rpc.active=activity([(1,[job(actor=OTHER)])])
        self.assertEqual(self.recovery.claim(self.folders[0]),(None,True))
        self.assertFalse((self.folders[0]/'current.json').exists())
    def test_identity_change_invalidates_the_negative_observation(self):
        self.rpc.active=activity([]);self.recovery.claim(self.folders[0])
        self.rpc.actor=OTHER;self.rpc.active=activity([(1,[job(actor=OTHER)])])
        self.rpc.exacts[report(1)['request_id']]=exact(1,[job(actor=OTHER)])
        audit,_=self.recovery.claim(self.folders[1]);self.assertEqual(audit['actor'],OTHER)
    def test_new_anchor_invalidates_negative_and_recovers_new_work(self):
        self.rpc.active=activity([]);self.recovery.claim(self.folders[0])
        self.rpc.tip='9'*64;self.rpc.active=activity([(1,[job()])]);self.rpc.active['state']['bestblockhash']=self.rpc.tip
        self.rpc.exacts[report(1)['request_id']]=exact(1,[job()],self.rpc.tip)
        self.assertIsNotNone(self.recovery.claim(self.folders[1])[0])
    def test_truncated_empty_for_actor_falls_back(self):
        self.rpc.active=activity([(1,[job(actor=OTHER)])]);self.rpc.active['truncated']=True
        self.rpc.active['totals'].update(active_reports=200,active_jobs=200)
        self.assert_fallback()
    def test_truncated_view_may_supply_verified_positive_work(self):
        self.rpc.active['truncated']=True;self.rpc.active['totals'].update(active_reports=200,active_jobs=200)
        self.assertIsNotNone(self.recovery.claim(self.folders[0])[0]);self.assertEqual(self.history_calls(),[])
    def test_unsupported_activity_preserves_history_discovery(self):
        self.rpc.unsupported=True
        self.rpc.history_jobs[report(10000)['request_id']]=[job()]
        audit,_=self.recovery.claim(self.folders[0]);self.assertEqual(audit['request_id'],report(10000)['request_id'])
        self.assertEqual(len(self.history_calls()),1)
    def test_inconsistent_totals_fall_back(self):
        self.rpc.active['totals']['selected_active_reports']=0;self.assert_fallback()
    def test_wrong_genesis_falls_back_without_cross_chain_adoption(self):
        self.rpc.active['state']['genesis']='f'*64;self.assert_fallback()
    def test_malformed_optional_state_falls_back(self):
        self.rpc.active['state']=None;self.assert_fallback()
    def test_malformed_exact_row_falls_back(self):
        self.rpc.exacts[report(1)['request_id']]['audits']=[None];self.assert_fallback()
    def test_exact_anchor_mismatch_falls_back(self):
        self.rpc.exacts[report(1)['request_id']]['state']['bestblockhash']='9'*64;self.assert_fallback()
    def test_exact_range_mismatch_falls_back(self):
        self.rpc.exacts[report(1)['request_id']]['audits'][0]['jobs'][0]['ranges'][0]['end']='1101'
        self.assert_fallback()
    def test_changed_tip_after_empty_view_falls_back_without_negative_shortcut(self):
        self.rpc.active=activity([]);self.rpc.flip_after_active=True
        self.assert_fallback()
    def test_reserved_actor_mismatch_falls_back_and_preserves_journal(self):
        value=self.reserve();value['actor']=OTHER
        (self.folders[1]/'current.json').write_text(json.dumps(value),encoding='utf8');self.assert_fallback()
        self.assertEqual(json.loads((self.folders[1]/'current.json').read_text()),value)
    def test_reserved_report_root_mismatch_falls_back(self):
        value=self.reserve();value['report_root']='7'*64
        (self.folders[1]/'current.json').write_text(json.dumps(value),encoding='utf8');self.assert_fallback()
    def test_conflicting_retained_progress_never_becomes_fast_absence(self):
        path=self.root/'progress.json';path.with_suffix('.conflict').write_text('retained')
        with patch.object(module,'progress_path',return_value=(path,None)):
            self.assert_fallback()
        self.assertEqual(path.with_suffix('.conflict').read_text(),'retained')
    def test_unresolved_legacy_seal_never_becomes_fast_absence(self):
        with patch.object(self.recovery,'retain_legacy_answer',side_effect=LegacyAuditSignerUnresolved('retained')):
            self.assert_fallback()
    def test_seal_owned_by_sibling_never_becomes_fast_absence(self):
        sibling = module.AuditAnswers(self.folders[1]).folder
        with patch.object(module.AuditAnswers,'records_seal',lambda lane,name: lane.folder==sibling):
            self.assert_fallback()
    def test_exact_lookup_budget_is_two_even_with_more_owned_reports(self):
        self.rpc.active=activity([(i,[job()])for i in (1,2,3)])
        self.rpc.exacts={report(i)['request_id']:exact(i,[job()])for i in (1,2,3)}
        with patch.object(self.recovery,'retain_legacy_answer',side_effect=LegacyAuditSignerUnresolved('retained')):
            self.assert_fallback()
        self.assertEqual(sum(m=='getpgnworkview'and len(args)==4 for m,args in self.rpc.calls),2)
    def test_terminal_or_expired_jobs_are_not_recovered(self):
        for state,expired in [('committed',False),('assigned',True)]:
            self.rpc.active=activity([(1,[dict(job(),state=state,expired=expired)])])
            if expired:
                # A terminal-only row is not a complete active-index projection.
                self.assert_fallback()
            else:
                self.assertEqual(self.recovery.claim(self.folders[0]),(None,True))
            self.rpc.calls=[];self.recovery.empty_observation=None

if __name__=='__main__':
    unittest.main(verbosity=2)
