"""Real disconnected Core: signed audit phases accepted in three blocks.

Controlled report counts test protocol transitions, not scientific computation,
Gold acceptance, CPU mining qualification or payment. No live profile is read.
"""
from pathlib import Path
from types import SimpleNamespace
import argparse
import base64
import copy
import hashlib
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'src/engine/runtime_host'), str(ROOT/'src/protocol')]
from horizon_core_rpc import CoreRpcError
from horizon_network_worker import NetworkWorker
from horizon_network_audits import NetworkAudits
from horizon_audit_batch import apply_group, reserve_assignments, commit_ready, save
from report_commitment import seal_report, digest, audit_commitment, canonical_bytes
from pgn_native_contract import fields

GENESIS = '710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'


class Node:
    def __init__(self, core, data):
        self.core, self.data = core, data
        self.audit_rpc_count = 0
        self.mock_time = None
        self.process = None
        self.log = None
        self.start()

    def start(self):
        self.data.mkdir(exist_ok=True)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        args = [str(self.core), '-testnet', '-server=1', '-listen=0', '-connect=0', '-dnsseed=0', '-fixedseeds=0', '-discover=0', '-listenonion=0', '-par=1', '-dbcache=32', '-keypool=2', '-rpcthreads=2', '-rpcbind=127.0.0.1', '-rpcallowip=127.0.0.1', '-rpcport='+str(self.port), '-datadir='+str(self.data)]
        if self.mock_time is not None:
            args.append('-mocktime='+str(self.mock_time))
        self.log = (self.data/'owned-core.log').open('ab')
        self.process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT,
                                        creationflags=0x08000000 if os.name == 'nt' else 0)
        until = time.monotonic()+30
        while self.process.poll() is None and time.monotonic() < until:
            try:
                assert self.call('getblockhash', [0]) == GENESIS
                return
            except (OSError, RuntimeError):
                time.sleep(.1)
        self.close()
        tail = (self.data/'owned-core.log').read_bytes()[-1200:].decode('utf-8',errors='replace')
        raise RuntimeError('Disposable Core readiness failed: '+tail)

    def call(self, method, params=None, wallet=False):
        if method == 'getpgnauditevent': self.audit_rpc_count += 1
        auth = base64.b64encode((self.data/'testnet/.cookie').read_bytes().strip()).decode()
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        try:
            connection.request('POST', '/wallet/batch232' if wallet else '/', json.dumps(dict(jsonrpc='2.0', id=1, method=method, params=params or [])), {'Content-Type':'application/json', 'Authorization':'Basic '+auth})
            value = json.loads(connection.getresponse().read(1048577))
            if value.get('error'):
                error = value['error']
                raise CoreRpcError(error['code'], error['message'])
            return value['result']
        finally:
            connection.close()

    def close(self):
        if self.process is not None:
            try:
                if self.process.poll() is None:
                    self.call('stop')
                    self.process.wait(timeout=15)
            finally:
                if self.process.poll() is None:
                    self.process.terminate()  # Only this owned test subprocess.
                    self.process.wait(timeout=10)
                self.log.close()


def worker(node, root, address, lock):
    root.mkdir(parents=True, exist_ok=True)
    value = object.__new__(NetworkWorker)
    value.root, value.rpc = root, node
    value.args = SimpleNamespace(pilot_coordination=True, genesis=GENESIS)
    value.network_lock = lock
    value.address = lambda: address
    value.quitting = threading.Event()
    value.running = threading.Event(); value.running.set()
    return value


def rejects(operation):
    try:
        operation()
    except (CoreRpcError, ValueError, RuntimeError) as error:
        return str(error)[:240]
    raise AssertionError('Invalid operation unexpectedly accepted')


def additional_protocol_report(node, publish, address):
    unsigned = node.call('getpgnticketrequest', [address])
    signature = node.call('signmessage', [address, unsigned['challenge']], True)
    signed = node.call('getpgnticketrequest', [address, None, signature, unsigned['anchor']])
    publish([signed['envelope']])
    # Exact unchanged genesis seed-delay relation: request height + 3.
    for _ in range(3): publish([])
    work = node.call('getpgnscientificwork', [signed['request_id'], '0'*64])
    start, end = int(work['start']), int(work['start'])+int(work['length'])
    body = dict(schema='PGN_HORIZON_COMPUTED_REPORT_V1', ticket_id=work['ticket'], start=work['start'],
        end=str(end), positions=work['length'], policy_sha256=fields(start,end)['threshold_policy']['sha256'],counts=['0']*100,best=[])
    sealed = seal_report(dict(body=body,sha256=digest(body),network_accepted=False),work,'e'*64)
    proposal = node.call('getpgnreportcommit',[work['request_id'],sealed['report_root']])
    signature = node.call('signmessage',[address,proposal['challenge']],True)
    publish([node.call('getpgnreportcommit',[work['request_id'],sealed['report_root'],signature])['envelope']])
    publish([])
    return work


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    node, result = None, {}
    with tempfile.TemporaryDirectory(prefix='pgn-audit-batch232-') as temporary:
        temp = Path(temporary)
        try:
            node = Node(args.core.resolve(), temp/'node')
            assert node.call('getblockcount') == 0 and node.call('getpeerinfo') == []
            node.call('createwallet', ['batch232'])
            roles = [node.call('getnewaddress', ['owned-protocol-fixture', 'bech32'], True) for _ in range(3)]
            lock = threading.RLock()
            owner = NetworkAudits(worker(node, temp/'owner', roles[0], lock))
            owner_worker = owner.worker
            def publish(envelopes):
                return owner_worker.ledger(envelopes)['block_hash']
            requests = []
            for address in roles:
                unsigned = node.call('getpgnticketrequest', [address])
                signature = node.call('signmessage', [address, unsigned['challenge']], True)
                requests.append(node.call('getpgnticketrequest', [address, None, signature, unsigned['anchor']]))
            publish([requests[0]['envelope']])
            # Recreate the two remaining requests at this actual parent.
            envelopes = []
            for address in roles[1:]:
                unsigned = node.call('getpgnticketrequest', [address])
                signature = node.call('signmessage', [address, unsigned['challenge']], True)
                envelopes.append(node.call('getpgnticketrequest', [address, None, signature, unsigned['anchor']])['envelope'])
            publish(envelopes)
            publish([]); publish([])
            work = node.call('getpgnscientificwork', [requests[0]['request_id'], '0'*64])
            start, end = int(work['start']), int(work['start'])+int(work['length'])
            body = dict(schema='PGN_HORIZON_COMPUTED_REPORT_V1', ticket_id=work['ticket'], start=work['start'], end=str(end), positions=work['length'], policy_sha256=fields(start,end)['threshold_policy']['sha256'], counts=['0']*100, best=[])
            sealed = seal_report(dict(body=body, sha256=digest(body), network_accepted=False), work, 'f'*64)
            unsigned = node.call('getpgnreportcommit', [work['request_id'], sealed['report_root']])
            signature = node.call('signmessage', [roles[0], unsigned['challenge']], True)
            publish([node.call('getpgnreportcommit', [work['request_id'], sealed['report_root'], signature])['envelope']])
            publish([])
            peers = [NetworkAudits(worker(node, temp/('lane'+str(i)), address, lock)) for i,address in enumerate(roles[1:])]
            for peer in peers:
                peer.worker.audit_publishers = tuple(peers)
                peer.worker.audit_group_journal = temp/'audit-assignment-group.json'
            absent = node.call('getpgnauditevent', ['assign', roles[1]])
            empty = node.call('getpgnauditevent', ['assign', roles[1], None, None, None, None, None, []])
            assert absent == empty
            result['empty_prefix_equals_absent'] = True
            old = [peer.event('assign') for peer in peers]
            assert [p['slot'] for p in old] == [0,0]
            result['baseline_collision'] = rejects(lambda: node.call('getpgnbootstraptemplate', [[p['envelope'] for p in old]]))
            # A forged prefix cannot reserve a slot for the next proposal.
            forged = old[0]['envelope'][:-2]+('00' if old[0]['envelope'][-2:] != '00' else '01')
            result['forged_signature_rejected'] = rejects(lambda: peers[1].event('assign', preceding_events=[forged]))
            result['explicit_slot_rejected'] = rejects(lambda: node.call('getpgnauditevent', ['assign', roles[1], work['request_id'], 1]))
            proper_a = peers[0].event('assign', preceding_events=[])
            proper_b = peers[1].event('assign', preceding_events=[proper_a['envelope']])
            assert [proper_a['slot'], proper_b['slot']] == [0,1]
            before_negative = node.call('getbestblockhash')
            result['incomplete_prefix_rejected'] = rejects(lambda: peers[1].event('assign', preceding_events=[proper_b['envelope']]))
            result['duplicated_prefix_rejected'] = rejects(lambda: peers[1].event('assign', preceding_events=[proper_a['envelope'], proper_a['envelope']]))
            result['reversed_order_rejected'] = rejects(lambda: node.call('getpgnbootstraptemplate', [[proper_b['envelope'], proper_a['envelope']]]))
            result['invalid_tail_has_no_partial_effect'] = rejects(lambda: node.call('getpgnbootstraptemplate', [[proper_a['envelope'], proper_b['envelope'], forged]]))
            assert node.call('getbestblockhash') == before_negative
            # Real local reorganization, not just a competing extension. The
            # exact original branch is reconsidered after the rejection probe.
            node.call('invalidateblock', [before_negative])
            reorg_tip = node.call('getbestblockhash')
            assert reorg_tip != before_negative
            result['reorganized_parent_rejected'] = rejects(lambda: peers[1].event('assign', preceding_events=[proper_a['envelope']]))
            assert node.call('getbestblockhash') == reorg_tip
            node.call('reconsiderblock', [before_negative])
            assert node.call('getbestblockhash') == before_negative
            # One genuine competing block invalidates the first group's
            # signatures; the complete group must be freshly proposed/signed.
            original = peers[0].worker.ledger
            parent_conflicts = []
            def competing_ledger(envelopes, **kwargs):
                if not parent_conflicts:
                    parent_conflicts.append(publish([]))
                return original(envelopes, **kwargs)
            peers[0].worker.ledger = competing_ledger
            base = node.call('getblockcount')+1  # Includes the deliberate competing block.
            gate = threading.Barrier(2)
            failures = []
            def reserve(peer):
                try:
                    gate.wait(timeout=5)
                    reserve_assignments(peer)
                except BaseException as error:
                    failures.append(error)
            import horizon_network_worker
            transport_save = horizon_network_worker.save
            interruption = {}
            def fail_shared(path, value):
                if Path(path) == peers[0].worker.root/'coordination-intent.json' and not interruption:
                    retained = json.loads((temp/'audit-assignment-group.json').read_bytes())
                    assert retained['intent'] == value
                    assert not Path(path).exists()
                    assert node.call('getbestblockhash') == value['parent']
                    interruption.update(hash=value['hash'],audit_rpc_count=node.audit_rpc_count)
                    raise OSError('injected interruption before shared candidate write')
                return transport_save(path,value)
            producers = [threading.Thread(target=reserve, args=(peer,)) for peer in peers]
            with patch('horizon_network_worker.save',new=fail_shared):
                for producer in producers: producer.start()
                for producer in producers:
                    producer.join(timeout=20)
                    assert not producer.is_alive()
            assert len(failures) == 1 and isinstance(failures[0],OSError) and str(failures[0]).startswith('injected interruption')
            assert interruption['hash'] == node.call('getbestblockhash')
            assert interruption['audit_rpc_count'] == node.audit_rpc_count
            result['group_only_journal_recovered_without_new_signature'] = True
            result['two_concurrent_producers_one_assignment_block'] = True
            peers[0].worker.ledger = original
            assignment_height = node.call('getblockcount')
            assert assignment_height == base+1
            assignment_hash = node.call('getbestblockhash')
            audits = [json.loads((peer.folder/'current.json').read_bytes()) for peer in peers]
            original_group = json.loads((temp/'audit-assignment-group.json').read_bytes())
            assert [a['slot'] for a in audits] == [0,1]
            assert len({(a['request_id'],a['slot'],a['attempt']) for a in audits}) == 2
            result['parent_conflict_rebased'] = True
            result['assignment_replay_rejected'] = rejects(lambda: node.call('getpgnbootstraptemplate', [[p['envelope'] for p in old]]))
            # Actual daemon restart, followed by idempotent lane recovery.
            node.close(); node.start(); node.call('loadwallet', ['batch232'])
            assert reserve_assignments(peers[1])['slot'] == 1
            assert node.call('getbestblockhash') == assignment_hash
            result['restart_preserved_two_reservations'] = True
            for index,(peer,audit) in enumerate(zip(peers,audits)):
                audit['answer'] = audit_commitment(audit['ticket'], audit['attempt'], [r['index'] for r in audit['ranges']], ['0']*len(audit['ranges']), str(index+1)*64)
                audit['commit_ready'] = True
                save(peer.folder/'current.json', audit)
            # Cannot use unconfirmed assignment prefix to construct a commit.
            result['cross_stage_rejected'] = rejects(lambda: peers[0].event('commit', *tuple(audits[0][k] for k in ('request_id','slot','attempt')), audits[0]['answer']['commitment'], preceding_events=[old[0]['envelope']]))
            commit = commit_ready(peers[0], audits[0], lambda intent: None)
            assert node.call('getblockcount') == assignment_height+1
            assert commit_ready(peers[1], audits[1], lambda intent: None)['confirmed_block'] == commit['confirmed_block']
            assert node.call('getblockcount') == assignment_height+1
            state = peers[0].state(work['request_id'])
            assert [j['state'] for j in state['jobs']] == ['committed','committed']
            # Early reveal without the owner's disclosure remains rejected.
            result['early_reveal_rejected'] = rejects(lambda: peers[0].event('reveal', *tuple(audits[0][k] for k in ('request_id','slot','attempt')), canonical_bytes(audits[0]['answer']['body']).hex()))
            report_path = owner.worker.root/'tickets'/work['ticket']/'network-report.json'
            report_path.parent.mkdir(parents=True)
            save(report_path,sealed)
            errors = owner.apply_reveal_group([(peer,str(i),dict(audit,_ready_owner_disclosure=True)) for i,(peer,audit) in enumerate(zip(peers,audits))])
            assert errors == []
            state = peers[0].state(work['request_id'])
            assert [j['state'] for j in state['jobs']] == ['passed','passed']
            assert node.call('getblockcount') == assignment_height+2
            assert node.call('verifychain', [4, 0]) is True
            assert node.call('getpeerinfo') == []
            blocks = [node.call('getblock', [node.call('getblockhash',[h]),2]) for h in range(assignment_height,assignment_height+3)]
            actual_events = [[output['scriptPubKey']['hex'] for tx in block['tx'] for output in tx['vout'] if b'PGNAUDIT-EVENT-1'.hex() in output['scriptPubKey'].get('hex','')] for block in blocks]
            assert [len(events) for events in actual_events] == [2,2,3]
            result.update(event_scripts_sha256=[[hashlib.sha256(bytes.fromhex(event)).hexdigest() for event in events] for events in actual_events])
            result.update(passed=True, genesis=GENESIS, core_sha256=hashlib.sha256(args.core.read_bytes()).hexdigest(), heights=list(range(assignment_height,assignment_height+3)), blocks=[b['hash'] for b in blocks], events=[2,2,3], phases=['two_assign','two_commit','owner_disclosure_and_two_reveal'], final_jobs=[dict(slot=j['slot'],attempt=j['attempt'],state=j['state']) for j in state['jobs']], verifychain=True, peer_count=0, protocol_fixture_only=True, scientific_computation_claimed=False, gold_or_payment_claimed=False, live_modified=False)
            result['three_block_sample_anchor'] = result['blocks'][-1]
            result['three_block_jobs'] = result.pop('final_jobs')
            # Later negative tests intentionally reorganize only this disposable
            # chain. Preserve the accepted three-block sample at its exact tip.
            seal_snapshot = {path: path.read_bytes() for peer in peers for path in peer.folder.glob('*.json')}
            late_owner = node.call('getnewaddress', ['owned-late-protocol-fixture','bech32'],True)

            def restored_group(label):
                folder = temp/label
                restored = [NetworkAudits(worker(node, folder/str(i), address, lock)) for i,address in enumerate(roles[1:])]
                journal = folder/'assignment-group.json'
                pending = copy.deepcopy(original_group);pending['retained'] = False
                save(journal,pending);raw = journal.read_bytes()
                for peer in restored:
                    peer.worker.audit_publishers = tuple(restored)
                    peer.worker.audit_group_journal = journal
                return restored, journal, raw

            def resolution(journal, raw, expected):
                name = hashlib.sha256(raw).hexdigest()
                history = journal.parent/'audit-assignment-history'
                assert (history/(name+'.json')).read_bytes() == raw
                found = [json.loads(path.read_bytes()) for path in history.glob(name+'-*.resolution.json')]
                assert len(found) == 1 and [row['status'] for row in found[0]['rows']] == [expected,expected]
                return found[0]

            later = additional_protocol_report(node,publish,late_owner)
            restored,journal,raw = restored_group('late-terminal')
            reserve_assignments(restored[0])
            assert all(json.loads((peer.folder/'current.json').read_bytes())['request_id'] == later['request_id'] for peer in restored)
            result['nonretained_passed_resumed_useful_work'] = resolution(journal,raw,'passed')

            # Existing CLI mocktime is process-local. No lease/consensus rule is
            # changed: six real blocks advance native MTP beyond the full24h.
            node.call('invalidateblock',[result['blocks'][1]])
            assert node.call('getbestblockhash') == assignment_hash
            before_expiry = peers[0].state(work['request_id'])
            deadline = max(job['deadline_time'] for job in before_expiry['jobs'])
            node.close();node.mock_time = deadline+3600;node.start();node.call('loadwallet',['batch232'])
            for _ in range(6): publish([])
            expired = peers[0].state(work['request_id'])
            assert all(job['state']=='assigned' and job['expired'] is True for job in expired['jobs'])
            expiry_anchor = node.call('getbestblockhash')
            assert node.call('getblockheader',[expiry_anchor,True])['mediantime'] >= deadline
            restored,journal,raw = restored_group('late-expired')
            reserve_assignments(restored[0])
            assert [json.loads((peer.folder/'current.json').read_bytes())['attempt'] for peer in restored] == [1,1]
            renewed = peers[0].state(work['request_id'])
            assert all(job['deadline_time']-job['issued_time']==86400 and job['expired'] is False for job in renewed['jobs'])
            result['nonretained_expired_resumed_new_native_attempt'] = resolution(journal,raw,'expired')
            result['expiration_clock'] = dict(method='Existing process-local CLI -mocktime, six accepted blocks; no source/rule change',deadline=deadline,mock_time=node.mock_time,anchor=expiry_anchor,median_time=node.call('getblockheader',[expiry_anchor,True])['mediantime'],lease_seconds=86400)

            later = additional_protocol_report(node,publish,late_owner)
            restored,journal,raw = restored_group('late-reassigned')
            reserve_assignments(restored[0])
            assert all(json.loads((peer.folder/'current.json').read_bytes())['request_id']==later['request_id'] for peer in restored)
            result['nonretained_reassigned_resumed_useful_work'] = resolution(journal,raw,'reassigned')
            assert all(path.read_bytes()==raw for path,raw in seal_snapshot.items())
            assert node.call('verifychain',[4,0]) is True and node.call('getpeerinfo')==[]
            result['seals_preserved_exact'] = True
            result['post_recovery_anchor'] = node.call('getbestblockhash')
            result['post_recovery_height'] = node.call('getblockcount')
            result['post_recovery_original_report_jobs'] = [dict(slot=job['slot'],attempt=job['attempt'],state=job['state']) for job in peers[0].state(work['request_id'])['jobs']]
            result['recovery_tests_reorganized_only_disposable_chain'] = True
        except BaseException as error:
            result['passed'] = False
            result['error_type'] = type(error).__name__
            result['error'] = str(error)[:500]
            raise
        finally:
            if node is not None:
                node.close()
                result['owned_process_closed'] = node.process.poll() is not None
            args.receipt.parent.mkdir(parents=True, exist_ok=True)
            args.receipt.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
