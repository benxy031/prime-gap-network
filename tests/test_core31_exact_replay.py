"""Run unchanged Core31 from an actual partial chainstate on both build platforms."""
from pathlib import Path,PurePosixPath
from contextlib import closing
import hashlib,http.client,io,json,os,re,shutil,subprocess,sys,time,urllib.request,urllib.error,zipfile
from native_grouped249_actual import Node,GENESIS
SOURCE='7d9a86019e6f131c4df53c0fabbd02047e8cfacc'
RUN=44200880416
REPO='wizz13150/prime-gap-network'
expected='014e8a77daae46db851f89f391faecf5a74c6c6d311ad81fb1f468f987bc1d5b'
platform='windows' if os.name=='nt' else 'ubuntu'
name=f'core31-{platform}-{RUN}'
out=Path('exact-native-qa').resolve()
out.mkdir()
assert os.environ.get('GITHUB_ACTIONS')=='true'
headers={'Accept':'application/vnd.github+json','User-Agent':'PGN-native-receipt','Authorization':'Bearer '+os.environ['GH_TOKEN']}
api=f'https://api.github.com/repos/{REPO}'
def request(url,maximum,authenticated=False):
    with urllib.request.urlopen(urllib.request.Request(url,headers=headers if authenticated else {'User-Agent':'PGN-native-receipt'}),timeout=30)as r:
        b=r.read(maximum+1)
    assert len(b)<=maximum
    return b
for i in range(21):
    assets=json.loads(request(f'{api}/actions/runs/{RUN}/artifacts',100000,True))['artifacts']
    matches=[a for a in assets if a['name']==name and not a['expired']]
    if matches:break
    time.sleep(16)
else:raise RuntimeError('Original exact native runtime artifact did not materialize')
asset,=matches
with closing(http.client.HTTPSConnection('api.github.com',timeout=20))as c:
    c.request('GET',f"/repos/{REPO}/actions/artifacts/{asset['id']}/zip",headers=headers)
    with c.getresponse()as response:
        assert response.status==302,response.status
        location=response.getheader('Location')
        assert location.startswith('https://')
raw=request(location,45*1024*1024)
assert asset['digest']=='sha256:'+hashlib.sha256(raw).hexdigest()
with zipfile.ZipFile(io.BytesIO(raw))as z:
    def single(leaf):
        matches=[item for item in z.infolist() if not item.is_dir() and PurePosixPath(item.filename).name==leaf]
        item,=matches
        assert 0<item.file_size<60*1024*1024
        return z.read(item)
    build=json.loads(single('BUILD.json'))
    assert build['source']==SOURCE and build['version']=='2026.11.17.31'
    runtime=out/'Core';runtime.mkdir()
    for filename,wanted in build['files'].items():
        assert filename==Path(filename).name
        binary=single(filename)
        assert len(binary)==wanted['bytes'] and hashlib.sha256(binary).hexdigest()==wanted['sha256']
        f=runtime/filename;f.write_bytes(binary)
        if os.name!='nt':f.chmod(0o755)
    migration=json.loads(single('MIGRATION.json'))
    assert migration['PASS']
    (out/'BUILD.json').write_text(json.dumps(build,indent=2)+'\n')
    (out/'MIGRATION.json').write_text(json.dumps(migration,indent=2)+'\n')
core=runtime/('pgn-core.exe' if os.name=='nt' else 'pgn-core')
version=subprocess.run([str(core),'-version'],capture_output=True,timeout=15)
assert version.returncode==0,(version.returncode,version.stderr.decode(errors='replace')[-1000:])
fixture=Path('qualification/native-restart/legacy-interrupted-public.zip')
assert hashlib.sha256(fixture.read_bytes()).hexdigest()==expected
partial=out/'partial'
partial.mkdir()
with zipfile.ZipFile(fixture)as z:
    description=json.loads(z.read('PARTIAL-FIXTURE.json'))
    assert description['public_data_only'] and description['no_wallets']
    assert description['genesis']==GENESIS and description['interrupted_height']==1164
    assert set(z.namelist())=={'PARTIAL-FIXTURE.json'}|set(description['files'])
    assert sum(i.file_size for i in z.infolist())<=40*1024*1024
    for relative,digest in description['files'].items():
        name=PurePosixPath(relative)
        assert name.parts[0]=='testnet' and name.parts[1] in {'blocks','chainstate','chainstate-pgn-request-history'}
        assert '..' not in name.parts and chr(92) not in relative and ':' not in relative
        content=z.read(relative)
        assert hashlib.sha256(content).hexdigest()==digest
        target=partial/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(content)
seed=Node(core,out/'seed')
wallet='QA_interrupted_replay'
try:
    assert seed.rpc('createwallet',[wallet,False,False,'',False,True])['name']==wallet
    address=seed.rpc('getnewaddress',['Pre-existing fixture wallet','bech32'])
    assert seed.rpc('getaddressinfo',[address])['ismine']
finally:seed.close()
shutil.copytree(out/'seed/testnet/wallets',partial/'testnet/wallets')
log=out/'INTERRUPTED-ONCE.log'
marker=b'Completing interrupted PGN chainstate validation through full replay of retained blocks'
with log.open('wb')as f:
    halted=subprocess.run([str(core),'-testnet=1',f'-datadir={partial}','-server=0',
        '-connect=0','-listen=0','-dnsseed=0','-fixedseeds=0','-stopatheight=1600','-par=1','-dbcache=64'],
        cwd=core.parent,stdout=f,stderr=subprocess.STDOUT,timeout=180)
assert log.stat().st_size<4*1024*1024
b=log.read_bytes()
assert halted.returncode==0,(halted.returncode,b[-1800:].decode(errors='replace'))
assert marker in b,b[-1800:].decode(errors='replace')
heights=[int(m[1]) for line in b.splitlines() if b'UpdateTip:' in line and (m:=re.search(rb'height=(\d+)',line))]
assert 1600<=max(heights)<2400,heights[-10:]
from native_grouped249_actual import port

class ReplayNode(Node):
    def start(self):
        self.rpcport=port()
        self.log=(self.data/'owned.log').open('ab')
        cursor=self.log.tell()
        args=[str(self.core),'-testnet','-server=1','-listen=0','-connect=0','-dnsseed=0',
              '-fixedseeds=0','-discover=0','-par=1','-dbcache=64','-keypool=2',
              '-rpcbind=127.0.0.1','-rpcallowip=127.0.0.1',f'-rpcport={self.rpcport}',f'-datadir={self.data}']
        self.process=subprocess.Popen(args,cwd=self.core.parent,stdout=self.log,stderr=subprocess.STDOUT,
              creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        started=time.monotonic()
        last_progress=started
        current_height=0
        tail=b''
        while self.process.poll() is None:
            try:
                if self.rpc('getblockhash',[0])==GENESIS:return
            except (OSError,RuntimeError):pass
            with (self.data/'owned.log').open('rb') as f:
                f.seek(cursor)
                tail=f.read(512*1024)
                cursor=f.tell()
            heights=[int(match[1]) for match in re.finditer(rb'height=(\d+)',tail)]
            now=time.monotonic()
            if max(heights,default=0)>current_height:
                current_height=max(heights);last_progress=now
            if now-started>245 or now-last_progress>85:break
            time.sleep(.22)
        exit_code=self.process.poll()
        if exit_code is None:
            self.process.terminate()
            self.process.wait(timeout=15)
        self.log.close()
        raise RuntimeError((exit_code,current_height,tail[-1800:].decode(errors='replace')))

def normal(name,prev_size,expect_replay):
    native_log=partial/'testnet/debug.log'
    node=ReplayNode(core,partial)
    try:
        deadline=time.monotonic()+220
        while time.monotonic()<deadline:
            chain=node.rpc('getblockchaininfo')
            if chain['blocks']>=description['canonical_height']:break
            time.sleep(.15)
        else:raise TimeoutError('Did not finish retained native replay')
        assert chain['blocks']==description['canonical_height']
        assert node.rpc('getblockhash',[chain['blocks']])==description['canonical_tip']
        if wallet not in node.rpc('listwallets'):node.rpc('loadwallet',[wallet])
        assert node.rpc('getaddressinfo',[address])['ismine']
    finally:node.close()
    assert 0<native_log.stat().st_size-prev_size<9*1024*1024
    with native_log.open('rb')as f:
        f.seek(prev_size)
        output=f.read(9*1024*1024)
    assert (marker in output)==expect_replay,name
    return native_log.stat().st_size
size=(partial/'testnet/debug.log').stat().st_size
closed=normal('first complete',size,True)
normal('cold normal restart',closed,False)
receipt=dict(PASS=True,base_build_source=SOURCE,base_run=RUN,artifact=asset['id'],digest=asset['digest'],
    partial_heights=[description['interrupted_height'],max(heights)],height=description['canonical_height'],
    wallet_unchanged=True,retained_tip_exact=True,automatic_native_replay=True,cold_restart_no_repeat=True,
    external_peers=False,disabled_validation=False)
(out/'RESTART-REPLAY.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
print(json.dumps(receipt))
