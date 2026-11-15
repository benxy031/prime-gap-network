"""Actual two-Qt-process/two-Core witness. Disposable localhost wallets only."""
from pathlib import Path
import base64, hashlib, json, os, queue, secrets, socket, subprocess, sys, threading, time, urllib.request, urllib.error

source = Path(__file__).resolve().parents[1]
base = source.parent
core = base.parent / 'updater-robert226/audit-batch233-public/artifacts/43680097035/audit-batch233-core-windows/Core/pgn-core.exe'
expected_core = 'cb6e003fd9815236779d1910f442e42015be54e9920b8d523be005e1c525fd8e'
assert hashlib.sha256(core.read_bytes()).hexdigest() == expected_core
client_exe = base / 'r211-build/pgn-social-native-witness.exe'
run = base / 'r211-evidence' / ('network-' + str(time.time_ns()))
run.mkdir(parents=True)
nodes, clients, checks = [], [], []
result = {'scope': 'Actual Core20 P2P + BIP322 signatures + product CoreSocialService/SocialView, disposable localhost only', 'core_sha256': expected_core, 'public_network': False, 'checks': checks}

def check(value, name, **facts):
    if not value:
        raise AssertionError(name)
    checks.append(dict(name=name, status='PASS', **facts))
    print('PASS ' + name, flush=True)

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def until(fn, label, timeout=35):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        last = fn()
        if last:
            return last
        time.sleep(.15)
    raise TimeoutError(label)

class Node:
    def __init__(self, label):
        self.label, self.path, self.rpc_port, self.p2p_port = label, run / ('core-' + label), port(), port()
        self.path.mkdir()
        self.wallet, self.cookie, self.process = 'social-r211-' + label, None, None
        self.log = (self.path / 'process.log').open('ab')
        nodes.append(self)
        self.start()
        self.rpc('createwallet', [self.wallet])
        self.address = self.rpc('getnewaddress', ['', 'bech32'], True)
        public = self.rpc('getaddressinfo', [self.address], True)
        assert public['ismine'] and public['scriptPubKey'].startswith('0014')
        self.script = public['scriptPubKey']
        self.genesis = self.rpc('getblockhash', [0])

    def start(self, reload_wallet=False):
        args = [str(core), '-testnet', '-datadir=' + str(self.path), '-server=1', '-listen=1', '-bind=127.0.0.1', '-port=' + str(self.p2p_port), '-connect=0', '-dnsseed=0', '-fixedseeds=0', '-discover=0', '-listenonion=0', '-pgnsocial=1', '-rpcbind=127.0.0.1', '-rpcallowip=127.0.0.1', '-rpcport=' + str(self.rpc_port), '-par=1', '-dbcache=32', '-maxconnections=16', '-keypool=2', '-printtoconsole=0']
        if reload_wallet:
            args.append('-wallet=' + self.wallet)
        self.process = subprocess.Popen(args, cwd=self.path, stdout=self.log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        def ready():
            if self.process.poll() is not None:
                raise RuntimeError('Owned Core exited: ' + self.label)
            cookies = list(self.path.glob('*/.cookie'))
            if cookies:
                self.cookie = cookies[0]
                try:
                    return self.rpc('getblockchaininfo')
                except (OSError, RuntimeError, ValueError):
                    pass
            return False
        until(ready, 'Owned Core startup ' + self.label)

    def rpc(self, method, params=None, wallet=False):
        auth = base64.b64encode(self.cookie.read_bytes().strip()).decode()
        url = 'http://127.0.0.1:' + str(self.rpc_port) + ('/wallet/' + self.wallet if wallet else '')
        data = json.dumps(dict(jsonrpc='2.0', id='social-r211', method=method, params=params or [])).encode()
        request = urllib.request.Request(url, data, {'Authorization': 'Basic ' + auth, 'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                value = json.load(reply)
        except urllib.error.HTTPError as error:
            value = json.load(error)
        if value.get('error'):
            raise RuntimeError(method + ': ' + json.dumps(value['error']))
        return value['result']

    def stop(self):
        if self.process and self.process.poll() is None:
            try:
                self.rpc('stop')
            except (OSError, RuntimeError):
                pass
            self.process.wait(timeout=25)

class Client:
    def __init__(self, node):
        self.node, self.sequence, self.responses = node, 0, queue.Queue()
        state = run / ('qt-' + node.label)
        state.mkdir()
        config = state / 'config.json'
        config.write_text(json.dumps(dict(label=node.label, state=str(state), rpc_port=node.rpc_port, cookie_file=str(node.cookie), wallet=node.wallet, genesis=node.genesis, script=node.script, address=node.address, device=secrets.token_hex(16))), encoding='utf-8')
        env = os.environ.copy()
        env['PATH'] = 'C:/Gapcoin/toolchain/msys64/ucrt64/bin;' + env['PATH']
        env['QT_QPA_PLATFORM'], env['QT_SCALE_FACTOR'] = 'windows', '1'
        self.log = (state / 'stderr.log').open('wb')
        self.process = subprocess.Popen([str(client_exe), str(config)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, env=env, text=True, encoding='utf-8')
        def reader():
            for line in self.process.stdout:
                try:
                    self.responses.put(json.loads(line))
                except ValueError:
                    self.responses.put({'harness_error': 'Non-JSON stdout'})
        threading.Thread(target=reader, daemon=True).start()
        clients.append(self)

    def command(self, name, **fields):
        self.sequence += 1
        self.process.stdin.write(json.dumps(dict(id=self.sequence, command=name, **fields)) + '\n')
        self.process.stdin.flush()
        response = self.responses.get(timeout=12)
        assert response.get('id') == self.sequence, response
        return response

    def has(self, text):
        reply = self.command('snapshot')
        return reply if not reply['error'] and any(m['body'] == text for m in reply['messages']) else False

    def stop(self):
        if self.process.poll() is None:
            self.command('stop')
            self.process.wait(timeout=10)
        self.log.close()

try:
    a, b = Node('A'), Node('B')
    check(a.genesis == b.genesis, 'Isolated clients share exact genesis')
    a.rpc('addnode', ['127.0.0.1:' + str(b.p2p_port), 'add'])
    until(lambda: a.rpc('getsocialpeers').get('relay_peers'), 'Native relay capability handshake')
    check(all(p['addr'].startswith('127.0.0.1:') for n in nodes for p in n.rpc('getpeerinfo')), 'Every actual peer is localhost')
    ca, cb = Client(a), Client(b)
    until(lambda: not ca.command('snapshot')['error'], 'Qt A native ready')
    until(lambda: not cb.command('snapshot')['error'], 'Qt B native ready')
    first, second = '[b]R211[/b] · Native Qt A → B', '[i]R211[/i] · Native Qt B → A'
    ca.command('send', text=first)
    received_b = until(lambda: cb.has(first), 'Actual Qt A-to-B receive')
    check(next(m for m in received_b['messages'] if m['body'] == first)['author_id'] == a.script, 'Received author is exact native signing script')
    cb.command('send', text=second)
    until(lambda: ca.has(second), 'Actual Qt B-to-A receive')
    check(True, 'Bidirectional native Qt/Core P2P exchange')
    def receipt_seen():
        rows = a.rpc('getsocialevents', ['', 0, 50])['records']
        return any(r['canonical_message'].startswith('PGN/SOCIAL/RECEIPT/1\n') for r in rows)
    until(receipt_seen, 'Native signed recipient receipt')
    check(True, 'Native signed recipient receipt persisted')
    # Populate actual signed messages until there is a real scroll range. The
    # fixture drives public Qt inputs, never the private bookmark implementation.
    for index in range(6):
        text = 'Isolated scroll check ' + str(index) + '\n' + '\n'.join('Signed network line ' + str(line) for line in range(8))
        ca.command('send', text=text)
        until(lambda: cb.has(text), 'Signed scroll population ' + str(index))
    def at_bottom(client):
        value = client.command('snapshot')
        return value if value['maximum'] > 0 and value['maximum'] - value['scroll'] <= 2 else False
    check(bool(until(lambda: at_bottom(cb), 'Incoming messages follow bottom')), 'Receiving at bottom follows new actual messages')
    cb.command('scroll_older')
    older = until(lambda: (lambda value: value if value['maximum'] > 0 and value['scroll'] == 0 else False)(cb.command('snapshot')), 'Actual Home-key old-history position')
    new_message = 'New actual message while B reads older history'
    ca.command('send', text=new_message)
    until(lambda: cb.has(new_message), 'Actual message while reading history')
    retained = until(lambda: (lambda value: value if value['unseen'] else False)(cb.command('snapshot')), 'New-message indicator')
    check(retained['anchor'] == older['anchor'] and abs(retained['anchor_top'] - older['anchor_top']) <= 2, 'Reading position preserved with new-message indicator', before=older['anchor_top'], after=retained['anchor_top'])
    cb.command('jump_latest')
    check(bool(until(lambda: at_bottom(cb), 'Jump to latest')), 'Actual new-message button jumps to latest')
    cb.command('scroll_older')
    own = 'Own signed reply from an old reading position'
    cb.command('send', text=own)
    until(lambda: ca.has(own), 'Own reply delivered')
    check(bool(until(lambda: at_bottom(cb), 'Own send follows bottom')), 'Own actual send returns to bottom')
    offline_draft = 'Offline draft retained under the same stable identity'
    b.stop()
    cb.command('draft', text=offline_draft)
    check(cb.command('draft_read')['text'] == offline_draft, 'Draft saved while owned Core is offline')
    b.start(reload_wallet=True)
    a.rpc('addnode', ['127.0.0.1:' + str(b.p2p_port), 'onetry'])
    cb.command('reopen')
    reconnected = until(lambda: cb.has(first), 'Stored native message after reconnection')
    check(cb.command('draft_read')['text'] == offline_draft, 'Draft survives actual service recreation')
    check(any(m['body'] == second for m in reconnected['messages']), 'Both directions retained after Core restart')
    ca.command('draft', text='Connected draft · Send is active. This text is not published.')
    check(ca.command('snapshot')['can_send'], 'Connected native Send is active before capture')
    for width, height in [(1366, 994), (1920, 1080)]:
        reply = ca.command('capture', width=width, height=height, path=str(run / ('chat-' + str(width) + '.png')))
        result.setdefault('capture_sizes', []).append(reply)
        check(reply['saved'] and (reply['width'], reply['height']) == (width, height), 'Actual native Qt chat capture ' + str(width), actual_width=reply['width'], actual_height=reply['height'])
    ca.command('leaderboard')
    for width, height in [(1366, 994), (1920, 1080)]:
        reply = ca.command('capture', width=width, height=height, path=str(run / ('leaderboard-' + str(width) + '.png')))
        check(reply['saved'], 'Actual native Qt leaderboard capture ' + str(width))
    result['native_counts'] = [c.command('snapshot') for c in clients]
    check(all(x['signed'] > 0 and x['verified'] > 0 for x in result['native_counts']), 'Both Qt clients signed and verified through actual Native BIP322')
    result['identities'] = [dict(script=n.script, genesis=n.genesis) for n in nodes]
    result['leaderboard'] = a.rpc('getpgnleaderboard', [0, 100])
    result['success'] = True
except Exception as error:
    result['success'], result['failure'] = False, repr(error)
    print('FAIL ' + repr(error), flush=True)
finally:
    for c in clients:
        try:
            if c.process.poll() is None:
                result.setdefault('final_qt_state', []).append(c.command('snapshot'))
            c.stop()
        except Exception as error:
            result.setdefault('cleanup_errors', []).append('Qt: ' + str(error))
    for n in nodes:
        try:
            if n.process.poll() is None:
                result.setdefault('final_peer_state', []).append(dict(label=n.label, peers=n.rpc('getpeerinfo'), social=n.rpc('getsocialpeers')))
            n.stop()
            n.log.close()
        except Exception as error:
            result.setdefault('cleanup_errors', []).append('Core: ' + str(error))
    result['all_processes_stopped'] = all(c.process.poll() is not None for c in clients) and all(n.process.poll() is not None for n in nodes)
    result['exit_codes'] = dict(qt=[c.process.poll() for c in clients], core=[n.process.poll() for n in nodes])
    (run / 'RESULT.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(str(run / 'RESULT.json'), flush=True)
sys.exit(0 if result.get('success') and result['all_processes_stopped'] and not result.get('cleanup_errors') else 1)
