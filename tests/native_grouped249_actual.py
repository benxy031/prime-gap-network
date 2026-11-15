"""Real Core, public prefix, real grouped coinbase, watch-only wallet and fork.

Only the qualification source pins activation at322. No live node or private
wallet is used; no mathematical report, expected audit or earlier block is made up.
"""
from pathlib import Path
from decimal import Decimal
import argparse,base64,contextlib,hashlib,http.client,json,os,socket,struct,subprocess,tempfile,time,urllib.parse
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
PAYOUT='00140b8cb565270a3ede7f09ae1ecdd7e0eaa7430e8d'
def sh(b):return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def file_sha(path):
    h=hashlib.sha256()
    with path.open('rb')as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    return h.hexdigest()
def port():
    with socket.socket()as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def vi(b,p):
    n=b[p];p+=1
    if n<253:return n,p
    width={253:2,254:4,255:8}[n];return int.from_bytes(b[p:p+width],'little'),p+width
class Node:
    def __init__(self,core,data):self.core=core;self.data=data;self.process=None;self.log=None;self.p2p=port();self.start()
    def start(self):
        self.data.mkdir(parents=True,exist_ok=True);self.rpcport=port();self.log=(self.data/'owned.log').open('ab')
        self.process=subprocess.Popen([str(self.core),'-testnet','-server=1','-listen=1','-bind=127.0.0.1','-port='+str(self.p2p),'-connect=0','-dnsseed=0','-fixedseeds=0','-discover=0','-listenonion=0','-par=1','-dbcache=32','-keypool=2','-rpcthreads=2','-rpcbind=127.0.0.1','-rpcallowip=127.0.0.1','-rpcport='+str(self.rpcport),'-datadir='+str(self.data)],stdin=subprocess.DEVNULL,stdout=self.log,stderr=subprocess.STDOUT,creationflags=0x08004000 if os.name=='nt' else 0)
        deadline=time.monotonic()+45
        while self.process.poll()is None and time.monotonic()<deadline:
            try:
                assert self.rpc('getblockhash',[0])==GENESIS;return
            except (OSError,RuntimeError):time.sleep(.1)
        raise RuntimeError('Owned test Core startup failed: '+(self.data/'owned.log').read_bytes()[-1200:].decode(errors='replace'))
    def rpc(self,method,params=None,wallet=False):
        auth=base64.b64encode((self.data/'testnet/.cookie').read_bytes().strip()).decode()
        c=http.client.HTTPConnection('127.0.0.1',self.rpcport,timeout=60)
        try:
            c.request('POST','/wallet/qa249' if wallet else '/',json.dumps(dict(jsonrpc='2.0',id=1,method=method,params=params or [])),{'Content-Type':'application/json','Authorization':'Basic '+auth})
            raw=c.getresponse().read(8000001);assert len(raw)<=8000000;v=json.loads(raw,parse_float=Decimal)
            if v.get('error'):raise RuntimeError(str(v['error']))
            return v['result']
        finally:c.close()
    def close(self):
        if self.process is None:return
        try:
            if self.process.poll()is None:self.rpc('stop');self.process.wait(timeout=45)
        finally:
            if self.process.poll()is None:self.process.terminate();self.process.wait(timeout=10)
            self.log.close();self.process=None
    def watch(self):
        self.rpc('createwallet',['qa249',True,True,'',False,True])
        decoded=self.rpc('decodescript',[PAYOUT]);descriptor=decoded.get('desc')
        assert isinstance(descriptor,str),decoded
        info=self.rpc('getdescriptorinfo',[descriptor]);r=self.rpc('importdescriptors',[[dict(desc=info['descriptor'],timestamp=0)]],True)
        assert len(r)==1 and r[0]['success'],r
    def coins(self):
        balances=self.rpc('getbalances',wallet=True)['mine']
        return sum(Decimal(str(balances[k]))for k in ('trusted','untrusted_pending','immature'))
    def ensure_wallet(self):
        if 'qa249'not in self.rpc('listwallets'):self.rpc('loadwallet',['qa249'])
def await_tip(node,wanted):
    until=time.monotonic()+20
    while time.monotonic()<until:
        if node.rpc('getbestblockhash')==wanted:return
        time.sleep(.05)
    raise AssertionError('Peer did not adopt expected branch')
def mine(raw,target,clock=None):
    b=bytearray(raw)
    if clock is not None:struct.pack_into('<q',b,68,clock)
    assert int.from_bytes(b[80:82],'little')==2
    for n in range(1000000):
        struct.pack_into('<Q',b,82,n);identity=sh(b[:112])
        if int.from_bytes(identity,'little')<=target:return bytes(b),identity[::-1].hex()
    raise RuntimeError('Owned coordination work search exhausted')
def legal_clock(node,parent,salt=0):
    header=node.rpc('getblockheader',[parent])
    # Exact Native contextual rules: >MTP and >=previous_time-15. Use the
    # retained history's valid time range rather than running100 blocks into
    # the real wall clock's separate15-second future bound. No mock-time.
    return max(header['mediantime']+1,header['time']-15)+salt
def append(node,salt=0):
    work=node.rpc('getpgnbootstraptemplate',[[]]);assert work['nonce_offset']==82 and work['nonce_bytes']==8
    proposed=bytes.fromhex(work['block']);clock=legal_clock(node,work['parent'],salt)
    raw,identity=mine(proposed,int(work['numeric_target'],16),clock)
    reply=node.rpc('submitblock',[raw.hex()]);assert reply is None,(work['height'],reply,clock)
    assert node.rpc('getbestblockhash')==identity
    return identity,raw
def tamper_first_payment(raw,target):
    b=bytearray(raw);count,p=vi(b,112);assert count==1;start=p;p+=4
    n,p=vi(b,p)
    for _ in range(n):p+=36;size,p=vi(b,p);p+=size+4
    n,p=vi(b,p);changed=False
    for _ in range(n):
        value=struct.unpack_from('<q',b,p)[0]
        if value>0 and not changed:struct.pack_into('<q',b,p,value+1);changed=True
        p+=8;size,p=vi(b,p);p+=size
    assert changed and p+4==len(b)
    b[36:68]=sh(b[start:])
    return mine(b,target)[0]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--core',type=Path,required=True);parser.add_argument('--history',type=Path,required=True);parser.add_argument('--receipt',type=Path,required=True);parser.add_argument('--legacy',action='store_true');args=parser.parse_args()
    output_count=38 if args.legacy else 1;format_version=5 if args.legacy else 6
    assert file_sha(args.history)=='09f318bfa826fad3d2af199ead9bb321dad039308864e549e3b92e6db6cfb7e4'
    nodes=[];result=dict(PASS=False,live_changed=False,public_activation=False,qualification_activation_height=322)
    started=time.monotonic()
    try:
        with contextlib.ExitStack()as cleanup:
            directory=cleanup.enter_context(tempfile.TemporaryDirectory(prefix='pgn-grouped249-'))
            root=Path(directory);a=Node(args.core.resolve(),root/'a');nodes.append(a);cleanup.callback(a.close)
            b=Node(args.core.resolve(),root/'b');nodes.append(b);cleanup.callback(b.close)
            with args.history.open('rb')as f:
                assert f.read(23)==b'PGN-PUBLIC-HISTORY-249\x00'
                for expected in range(322):
                    height,size=struct.unpack('<II',f.read(8));identity=f.read(32).hex();raw=f.read(size)
                    assert height==expected and len(raw)==size
                    for node in nodes:
                        if height:assert node.rpc('submitblock',[raw.hex()])is None,(height,node.rpc('getbestblockhash'))
                        assert node.rpc('getblockhash',[height])==identity
            for node in nodes:
                s=node.rpc('getpgnstate',[0,1]);assert s['height']==321 and s['gold_blocks']==10 and s['audit_paid_atomic']=='0'and s['audit_pool_atomic']=='1250000000000'and s['issuance_snapshot_version']==5
                node.watch();assert node.coins()==0
            result['phase']='public_prefix_and_watch_wallets_passed'
            b.rpc('addnode',['127.0.0.1:'+str(a.p2p),'onetry'])
            work=a.rpc('getpgnbootstraptemplate',[[]]);assert work['height']==322
            raw,identity=mine(bytes.fromhex(work['block']),int(work['numeric_target'],16),legal_clock(a,work['parent']))
            invalid=tamper_first_payment(raw,int(work['numeric_target'],16));reject=a.rpc('submitblock',[invalid.hex()]);assert isinstance(reject,str) and reject
            assert a.rpc('getblockcount')==321 and a.rpc('getpgnstate',[0,1])['audit_paid_atomic']=='0'
            assert a.rpc('submitblock',[raw.hex()])is None;await_tip(b,identity)
            block=a.rpc('getblock',[identity,2]);positive=[o for o in block['tx'][0]['vout']if o['value']]
            assert len(positive)==output_count and sum(Decimal(str(o['value']))for o in positive)==125
            for node in nodes:
                s=node.rpc('getpgnstate',[0,1]);assert s['gold_blocks']==10 and s['audit_paid_atomic']=='1250000000000'and s['audit_pool_atomic']=='0'and s['issuance_snapshot_version']==format_version
                assert node.coins()==125
            result['phase']='actual_payment_and_peer_relay_passed'
            next_hash,_=append(a);await_tip(b,next_hash)
            assert not any(o['value']for o in a.rpc('getblock',[next_hash,2])['tx'][0]['vout'])
            a.close();a.start();a.ensure_wallet();assert a.coins()==125
            a.rpc('invalidateblock',[identity]);assert a.rpc('getblockcount')==321 and a.coins()==0
            assert a.rpc('getpgnstate',[0,1])['issuance_snapshot_version']==5
            # New actual branch, then enough Native work for the other peer to reorganize.
            for salt in (1,2,3):fork_hash,_=append(a,salt)
            b.rpc('addnode',['127.0.0.1:'+str(a.p2p),'onetry']);await_tip(b,fork_hash)
            assert b.rpc('getblock',[identity])['confirmations']==-1
            for node in nodes:assert node.coins()==125 and node.rpc('getpgnstate',[0,1])['audit_paid_atomic']=='1250000000000'
            result['phase']='cold_restart_and_actual_two_node_reorg_passed'
            for _ in range(101):last,_=append(a)
            await_tip(b,last)
            for node in nodes:
                unspent=node.rpc('listunspent',[1,9999999,[],True],True)
                assert len(unspent)==output_count and sum(Decimal(str(o['amount']))for o in unspent)==125 and all(o['scriptPubKey']==PAYOUT for o in unspent)
                assert node.rpc('verifychain',[4,0])is True
                node.close();node.start();node.ensure_wallet();assert node.coins()==125 and node.rpc('getbestblockhash')==last
            result.update(PASS=True,public_prefix_blocks=322,actual_original_audit_outputs=38,actual_payment_outputs=output_count,legacy_reference=args.legacy,exact_paid_PGN='125',two_Core_relay=True,full_ConnectBlock=True,watch_only_wallets=True,malformed_amount_rejected=reject,duplicate_payment_refused=True,cold_restart=True,reorg_undo_and_reapply=True,mature_UTXOs=output_count,verifychain_level4=True,core_sha256=hashlib.sha256(args.core.read_bytes()).hexdigest(),seconds=round(time.monotonic()-started,3))
    except Exception as error:
        result['error']=str(error)[:2000]
        raise
    finally:
        for node in reversed(nodes):node.close()
        args.receipt.parent.mkdir(parents=True,exist_ok=True);args.receipt.write_text(json.dumps(result,indent=2,default=str)+'\n');print(json.dumps(result,default=str),flush=True)
if __name__=='__main__':main()
