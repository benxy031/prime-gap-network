"""Real disconnected Core: signed audit phases accepted in three blocks.

Controlled report counts test protocol transitions, not scientific computation,
Gold acceptance, CPU mining qualification or payment. No live profile is read.
"""
from pathlib import Path
from types import SimpleNamespace
import argparse
import base64
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
        self.process = None
        self.log = None
        self.start()

    def start(self):
        self.data.mkdir(exist_ok=True)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        args = [str(self.core), '-testnet', '-server=1', '-listen=0', '-connect=0', '-dnsseed=0', '-fixedseeds=0', '-discover=0', '-listenonion=0', '-par=1', '-dbcache=32', '-keypool=2', '-rpcthreads=2', '-rpcbind=127.0.0.1', '-rpcallowip=127.0.0.1', '-rpcport='+str(self.port), '-datadir='+str(self.data)]
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
        raise RuntimeError('Disposable Core readiness failed')

    def call(self, method, params=None, wallet=False):
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
            old = [peer.event('assign') for peer in peers]
            assert [p['slot'] for p in old] == [0,0]
            result['baseline_collision'] = rejects(lambda: node.call('getpgnbootstraptemplate', [[p['envelope'] for p in old]]))
            # A forged prefix cannot reserve a slot for the next proposal.
            forged = old[0]['envelope'][:-2]+('00' if old[0]['envelope'][-2:] != '00' else '01')
            result['forged_signature_rejected'] = rejects(lambda: peers[1].event('assign', preceding_events=[forged]))
            result['explicit_slot_rejected'] = rejects(lambda: node.call('getpgnauditevent', ['assign', roles[1], work['request_id'], 1]))
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
            reserve_assignments(peers[0])
            peers[0].worker.ledger = original
            assignment_height = node.call('getblockcount')
            assert assignment_height == base+1
            assignment_hash = node.call('getbestblockhash')
            audits = [json.loads((peer.folder/'current.json').read_bytes()) for peer in peers]
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
            third = apply_group([(owner,'reveal_report',work['request_id'],0,0,canonical_bytes(sealed['body']).hex())]+[(peer,'reveal',audit['request_id'],audit['slot'],audit['attempt'],canonical_bytes(audit['answer']['body']).hex()) for peer,audit in zip(peers,audits)])
            assert len({p['confirmed_block'] for p in third}) == 1
            state = peers[0].state(work['request_id'])
            assert [j['state'] for j in state['jobs']] == ['passed','passed']
            assert node.call('getblockcount') == assignment_height+2
            assert node.call('verifychain', [4, 0]) is True
            assert node.call('getpeerinfo') == []
            blocks = [node.call('getblock', [node.call('getblockhash',[h]),2]) for h in range(assignment_height,assignment_height+3)]
            result.update(passed=True, genesis=GENESIS, core_sha256=hashlib.sha256(args.core.read_bytes()).hexdigest(), heights=list(range(assignment_height,assignment_height+3)), blocks=[b['hash'] for b in blocks], events=[2,2,3], phases=['two_assign','two_commit','owner_disclosure_and_two_reveal'], final_jobs=[dict(slot=j['slot'],attempt=j['attempt'],state=j['state']) for j in state['jobs']], verifychain=True, peer_count=0, protocol_fixture_only=True, scientific_computation_claimed=False, gold_or_payment_claimed=False, live_modified=False)
        finally:
            if node is not None:
                node.close()
                result['owned_process_closed'] = node.process.poll() is not None
            args.receipt.parent.mkdir(parents=True, exist_ok=True)
            args.receipt.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
