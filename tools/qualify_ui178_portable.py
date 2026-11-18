"""Exact UI178 portable startup against a retained test wallet on an isolated Windows runner."""
from pathlib import Path
import hashlib,http.client,json,os,re,subprocess,sys,zipfile,time,urllib.request,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from shutil import copytree
from contextlib import closing,contextmanager
from pathlib import PurePosixPath
from shutil import disk_usage
from urllib3.exceptions import ProtocolError
assert os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='nt'
ROOT=Path.cwd();QA=ROOT/'ui178-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='067ef90632d4d1dba5ba515d7594ac3680d3e4fa8b6c4c7f3ef3eea70762dc24'
URL='https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Windows-177.zip'
archive=QA/'baseline.zip';urllib.request.urlretrieve(URL,archive)
def sha(p):
    with Path(p).open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(archive)==BASE_SHA
with zipfile.ZipFile(archive)as z:z.extractall(QA/'baseline')
manifests=list((QA/'baseline').rglob('components.json'));assert len(manifests)==1
base=manifests[0].parent.resolve();exe=base/'PGN-Horizon.exe'
assert not (base/'SOURCE-COMMIT.json').exists(), 'Publisher provenance must remain outside code'
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'src/update'),str(ROOT/'tests')]
import stage_component_bundle as stage
from horizon_update_client import AuthenticatedUpdates
from horizon_update_progress import ResumableTargetFetcher
from horizon_component_install import CohortStore,inventory
from horizon_windows_install import WindowsCohortLifecycle
from horizon_windows_lifecycle import OwnedWindowsProcess,owned_ipc,ProcessAbsentError
from native_grouped249_actual import Node,port
from qualify_portable_slow_core_windows import rpc,stop_fixture_core
WALLET='QA_Updater_Retained_Wallet'
roaming=(QA/'Roaming').resolve()
local=(QA/'Local').resolve()
roaming.mkdir();local.mkdir()
os.environ.update(APPDATA=str(roaming),LOCALAPPDATA=str(local))
wallet_data=roaming/'PGNCore'/'networks'/GENESIS[:16]
fixture=ROOT/'qualification/ui177/legacy-chain.zip'
assert sha(fixture)=='33f5d5a206786715725ffbed770bfeba3b014898bf6c0e734419fbbb62a050c6'
def install_public_fixture(data_directory):
    with zipfile.ZipFile(fixture) as old_chain:
        assert sum(info.file_size for info in old_chain.infolist())<128*1024*1024
        for info in old_chain.infolist():
            name=PurePosixPath(info.filename)
            assert not name.is_absolute() and '..' not in name.parts and 'wallets' not in name.parts
            assert chr(92) not in info.filename and ':' not in info.filename
            path=data_directory/name
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(old_chain.read(info))
    legacy=json.loads((data_directory/'FIXTURE.json').read_bytes())
    assert legacy['public_data_only'] and legacy['no_wallets']
    assert legacy['genesis']==GENESIS and legacy['legacy_height']==4245 and legacy['canonical_height']==5681
    for name,signature in legacy['files'].items():
        assert sha(data_directory/name)==signature
    return legacy

def wait_native_tip(node, height, timeout):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline and node.process.poll() is None:
        chain=node.rpc('getblockchaininfo')
        if chain['blocks']>=height:
            return chain['blocks']
        time.sleep(.12)
    raise AssertionError(f'Expected retained tip {height}; exit {node.process.poll()}')

def make_wallet(node):
    assert node.rpc('createwallet',[WALLET,False,False,'',False,True])['name']==WALLET
    address=node.rpc('getnewaddress',['Updater fixture','bech32'])
    assert node.rpc('getaddressinfo',[address])['ismine'] is True
    return address

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
        log_path=self.data/'owned.log'
        while self.process.poll() is None:
            try:
                if self.rpc('getblockhash',[0])==GENESIS:return
            except (OSError,RuntimeError):pass
            with log_path.open('rb') as f:
                f.seek(cursor)
                emitted=f.read(512*1024)
                cursor=f.tell()
            if emitted:
                tail=(tail+emitted)[-32768:]
            heights=[int(match[1]) for match in
                     re.finditer(rb'(?:height=|verification at block #)(\d+)',emitted)]
            now=time.monotonic()
            if max(heights,default=0)>current_height:
                current_height=max(heights);last_progress=now
            if now-started>390 or now-last_progress>140:break
            time.sleep(.3)
        exit_code=self.process.poll()
        if exit_code is None:
            self.process.terminate()
            self.process.wait(timeout=15)
        self.log.close()
        native=self.data/'testnet/debug.log'
        if native.is_file():
            with native.open('rb') as f:
                f.seek(max(0,native.stat().st_size-49152))
                (E/'NATIVE-RESTART-TAIL.log').write_bytes(f.read(49152))
        (E/'NATIVE-STDOUT-TAIL.log').write_bytes(tail)
        raise RuntimeError((exit_code,current_height,tail[-1800:].decode(errors='replace')))

def open_existing_core(data_directory):
    return ReplayNode(base/'Core/pgn-core.exe',data_directory)


candidate=list((ROOT/'ui178-qualified').rglob('PGN-Horizon.exe'))
assert len(candidate)==1
candidate=candidate[0]
assert sha(candidate)=='246a3b18bae378b738c45a0983cfe420b22a361f17dcbbf3a74f0b59c63f4130'
assert not exe.is_symlink()
manifest_path=base/'components.json'
manifest=json.loads(manifest_path.read_bytes())
assert {c['id']:c['version'] for c in manifest['components']}=={'ui': '2026.11.17.177', 'core': '2026.11.17.31', 'gpu': '2026.11.17.87', 'cpu': '2026.11.6.389'}
ui,=[c for c in manifest['components'] if c['id']=='ui']
changes={
    'PGN-Horizon.exe':candidate.read_bytes(),
    'README.txt':(base/'README.txt').read_bytes().replace(b'UI177',b'UI178',1),
}
assert b'UI178' in changes['README.txt']
ui['version']=manifest['release_version']='2026.11.18.178'
for row in ui['files']:
    if row['path'] in changes:
        name=row['path']
        (base/name).write_bytes(changes[name])
        row['bytes']=len(changes[name])
        row['sha256']=sha(base/name)
manifest_path.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
assert sha(manifest_path)=='248ffecd021509846902fa7cbd895ec820a73fd24a818c58b3c00b0b0d025db4'
_,files,_,_=inventory(base)
assert files['pgn-horizon.exe']['sha256']=='246a3b18bae378b738c45a0983cfe420b22a361f17dcbbf3a74f0b59c63f4130'
assert files['core/pgn-core.exe']['sha256']=='3df925c084ad863521329fe438c804efa7d81905f7d350ad41981efa6402f25e'

legacy=install_public_fixture(wallet_data)
node=open_existing_core(wallet_data)
try:
    assert wait_native_tip(node,legacy['canonical_height'],240)==legacy['canonical_height']
    assert node.rpc('getblockhash',[legacy['canonical_height']])==legacy['canonical_tip']
    address=make_wallet(node)
finally:
    node.close()
assert (wallet_data/'testnet/wallets'/WALLET/'wallet.dat').is_file()
with (wallet_data/'pgn.conf').open('a',encoding='utf-8') as f:
    f.write('networkactive=0\n')
wallet_port=json.loads((base/'Core/core-template.json').read_bytes())['deployment']['rpc_port']

with (E/'startup.log').open('wb') as output:
    started=time.monotonic()
    process=subprocess.Popen([str(exe),'--start-minimized'],cwd=base,stdout=output,stderr=subprocess.STDOUT,creationflags=0x08000000)
owner=OwnedWindowsProcess(process.pid,exe,sha(exe))
def save(name,result):
    (E/name).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
state={}
try:
    deadline=time.monotonic()+150
    while time.monotonic()<deadline and process.poll() is None:
        try:
            state=owned_ipc(owner,'--inspect-owned')
            if state.get('pgn_core_link_state')=='ready' and state.get('pgn_network_height',0)>=legacy['canonical_height']:
                break
        except (OSError,RuntimeError,ValueError):
            pass
        time.sleep(1)
    assert state.get('pgn_core_link_state')=='ready',{'status':state.get('pgn_core_link_state'),'process_exit':process.poll()}
    assert state['pgn_update_genesis']==GENESIS and state['pgn_wallet_embedded_pid']>0
    assert Path(state['pgn_update_application_root']).resolve()==base
    assert not Path(state['pgn_update_data_root']).resolve().is_relative_to(base)
    assert rpc(wallet_data,wallet_port,'listwallets',[])==[WALLET]
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
    assert rpc(wallet_data,wallet_port,'getblockhash',[legacy['canonical_height']])==legacy['canonical_tip']
    readiness=dict(PASS=True,ui='2026.11.18.178',exe=sha(exe),manifest=sha(manifest_path),
        previous_height=legacy['canonical_height'],wallet_auto_attached=True,ordinary_portable_start=True,
        boot_ms=round((time.monotonic()-started)*1000),core_unchanged=True)
    save('STARTUP-PASS.json',readiness)
    print('Ordinary Windows startup with retained test wallet passed',flush=True)
    rpc(wallet_data,wallet_port,'setnetworkactive',[True])
    deadline=time.monotonic()+180
    while time.monotonic()<deadline and process.poll() is None:
        try:
            state=owned_ipc(owner,'--inspect-owned')
            chain=rpc(wallet_data,wallet_port,'getblockchaininfo',[],timeout=8)
            height=min(int(state.get('pgn_network_height',0)),chain['blocks'])
            if height>legacy['canonical_height']:
                assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
                save('CATCHUP-PASS.json',readiness|dict(height=height,peers=len(rpc(wallet_data,wallet_port,'getpeerinfo',[]))))
                print(f'Exact retained wallet caught up to {height}',flush=True)
                break
        except (OSError,RuntimeError,ValueError):
            pass
        time.sleep(1)
    else:
        raise AssertionError('Retained fixture did not advance on the public network in the isolated run')
finally:
    if not owner.exited():
        owned_ipc(owner,'--close-owned')
    process.wait(timeout=30)
