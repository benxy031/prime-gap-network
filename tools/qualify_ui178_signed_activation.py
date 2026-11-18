"""Actual public TUF download, cohort install and real Qt restart on isolated Windows."""
from pathlib import Path
import hashlib,http.client,json,os,re,subprocess,sys,zipfile,time,urllib.request,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from shutil import copytree
from contextlib import closing,contextmanager
from pathlib import PurePosixPath
from shutil import disk_usage
from urllib3.exceptions import ProtocolError
assert os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='nt'
ROOT=Path.cwd();QA=ROOT/'ui178-signed-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
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
        self.data.mkdir(parents=True,exist_ok=True)
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


from urllib.parse import unquote, urlsplit
from tuf.api.exceptions import DownloadHTTPError
from tuf.ngclient.fetcher import FetcherInterface

BUNDLE_SHA='964510c1276de9832f232aebfeb3b87582a9a7e1967fc4ec62c63856e6bfe0ee'
ROOT_SHA='37f8978ea7e5844bb45b3f22c5cf2f51b68266f26a92583a4959a9e9a3870dbf'
UI_SHA='03a298c14cc814e20e44649bb71837a4525c2b823f416a87a7d84a95aa99339d'
CORE_SHA='3df925c084ad863521329fe438c804efa7d81905f7d350ad41981efa6402f25e'
draft=ROOT/'signed-candidate-46'
signed=draft/'metadata-unpacked'
signed.mkdir()
with zipfile.ZipFile(draft/'qa-catalogue46-metadata.zip')as z:
    members=z.infolist()
    assert len(members)==8
    for entry in members:
        name=PurePosixPath(entry.filename)
        assert len(name.parts)==2 and name.parts[0] in ('metadata','targets')
        assert name.name.endswith('.json') and not any(x in entry.filename for x in (':','\\','..'))
        assert 0<entry.file_size<128*1024
        path=signed/name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(z.read(entry))
bundle=draft/(BUNDLE_SHA+'.components-46.zip')
assert bundle.stat().st_size==267873856 and sha(bundle)==BUNDLE_SHA
root=(signed/'metadata/root.json').read_bytes()
assert hashlib.sha256(root).hexdigest()==ROOT_SHA

class ExactDraftFetcher(FetcherInterface):
    def __init__(self):
        self.bundle_requests=0
    def _fetch(self,url):
        parsed=urlsplit(url)
        assert parsed.scheme=='https' and parsed.hostname=='windows-qualification.invalid'
        folder,name=unquote(parsed.path).strip('/').split('/')
        assert folder in ('metadata','targets') and name and not any(x in name for x in (':','\\','..'))
        if name.endswith('.zip'):
            assert folder=='targets' and name==bundle.name
            self.bundle_requests+=1
            path=bundle
        else:
            path=signed/folder/name
        if not path.is_file():
            raise DownloadHTTPError('No exact preview metadata for the requested version',404)
        with path.open('rb') as stream:
            while chunk:=stream.read(1024*1024):yield chunk

def save(name, data):
    (E/name).write_text(json.dumps(data,indent=2)+'\n',encoding='utf8')

def wait_wallet(owner,timeout):
    deadline=time.monotonic()+timeout
    detail=''
    while time.monotonic()<deadline and not owner.exited():
        try:
            state=owned_ipc(owner,'--inspect-owned')
            detail=str(state.get('pgn_core_link_detail',''))[:240]
            if state.get('pgn_core_link_state')=='ready' and state.get('pgn_wallet_embedded_pid'):
                if rpc(wallet_data,wallet_port,'listwallets',[])==[WALLET]:
                    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
                    return state
        except (OSError,RuntimeError,ValueError):
            pass
        time.sleep(.5)
    raise RuntimeError(f'Owned GUI did not attach the retained test wallet: {detail}')

def close_gui(owner):
    if owner is None or owner.exited():return
    run=subprocess.run([str(owner.executable),'--close-owned',str(owner.pid)],capture_output=True,timeout=20)
    assert run.returncode==0
    deadline=time.monotonic()+30
    while not owner.exited() and time.monotonic()<deadline:time.sleep(.25)
    assert owner.exited()

legacy=install_public_fixture(wallet_data)
node=open_existing_core(wallet_data)
try:
    assert wait_native_tip(node,legacy['canonical_height'],240)==legacy['canonical_height']
    assert node.rpc('getblockhash',[legacy['canonical_height']])==legacy['canonical_tip']
    address=make_wallet(node)
finally:
    node.close()
assert (wallet_data/'testnet/wallets'/WALLET/'wallet.dat').is_file()
with (wallet_data/'pgn.conf').open('a',encoding='utf-8') as f:f.write('networkactive=0\n')
wallet_port=json.loads((base/'Core/core-template.json').read_bytes())['deployment']['rpc_port']
old,files,old_digest,_=inventory(base)
old_versions={c['id']:c['version'] for c in old['components']}
assert old_versions==dict(ui='2026.11.17.177',core='2026.11.17.31',gpu='2026.11.17.87',cpu='2026.11.6.389')
assert sha(base/'Core/pgn-core.exe')==CORE_SHA
for name in ('QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH'):os.environ.pop(name,None)
with (E/'baseline.log').open('wb') as out:
    process=subprocess.Popen([str(exe),'--testnet','--start-minimized','--update-healthcheck'],cwd=base,
          stdout=out,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
owner=OwnedWindowsProcess(process.pid,exe,files['pgn-horizon.exe']['sha256'])
lifecycle=None
try:
    state=wait_wallet(owner,150)
    assert state['pgn_update_genesis']==GENESIS
    data=Path(state['pgn_update_data_root']).resolve()
    assert not data.is_relative_to(base)
    old_core_pid=state['pgn_wallet_embedded_pid']
    sentinel=data/'exact-test-data-sentinel.bin'
    sentinel.parent.mkdir(parents=True,exist_ok=True)
    sentinel.write_bytes(b'PGN retained test data')
    initial=sha(sentinel)
    context,context_hash=stage.context_from_bytes(json.dumps(dict(
        schema='PGN_COMPONENT_STAGE_CONTEXT_V1',network='testnet',genesis_hash=GENESIS,
        platform='windows-x86_64',compatibility_set='pgn.epoch3.2026.11',minimum_sequence=44,
        installed=[dict(id=c['id'],abi='pgn.epoch3.2026.11',version=c['version'])for c in old['components']],
        protected_roots=[str(base),str(data)])).encode())
    fetcher=ExactDraftFetcher()
    client=AuthenticatedUpdates(QA/'downloads',root,ROOT_SHA,'https://windows-qualification.invalid/metadata/',
        'https://windows-qualification.invalid/targets/',context,context_hash,fetcher=fetcher)
    store=CohortStore.initialize(QA/'installation',base,context,context_hash)
    previous=store.current()
    requested=['ui','gpu']
    assert client.refresh(requested)['sequence']==46
    candidate=store.prepare(client,requested,retain_download=True)
    assert fetcher.bundle_requests==1 and store.current()==previous and not owner.exited()
    assert client.refresh(requested)['sequence']==46
    repeat=store.prepare(client,requested)
    assert fetcher.bundle_requests==1 and repeat['cohort']==candidate['cohort']
    assert repeat['inventory_sha256']==candidate['inventory_sha256']
    new=Path(candidate['cohort'])
    manifest,declared,new_digest,_=inventory(new)
    versions={c['id']:c['version'] for c in manifest['components']}
    assert len(declared)==424 and new_digest=='9ecf290478777dc6192d1596dfddb7376521288e006597bdc47594a1658b762a'
    assert versions==dict(ui='2026.11.18.178',core='2026.11.17.31',gpu='2026.11.18.88',cpu='2026.11.6.389')
    assert sha(new/'PGN-Horizon.exe')==UI_SHA and sha(new/'Core/pgn-core.exe')==CORE_SHA
    command=lambda tool,*args:json.loads(subprocess.check_output([str(new/tool),*args],timeout=40))
    helper=command('UpdateRuntime/horizon-updater.exe','--version')
    host=command('EngineRuntime/network/horizon-engine-host.exe','--audit-batch-self-test')
    assert helper['version']=='2026.11.18.32' and host['passed'] and host['checks']==17
    save('PREPARE-PASS.json',dict(PASS=True,sequence=46,files=len(declared),manifest_sha256=new_digest,
        before_sequence=previous['sequence'],bundle_transfers=fetcher.bundle_requests,versions=versions))
    activation=dict(schema='PGN_WINDOWS_UPDATE_ACTIVATION_V1',
        ui_pid=owner.pid,ui_birth=owner.birth,application_root=state['pgn_update_application_root'],data_root=str(data))
    lifecycle=WindowsCohortLifecycle(store,base,activation)
    assert store.activate(candidate,lifecycle.drain,lifecycle.start_and_verify,lifecycle.restore_previous)
    process.wait(timeout=20);assert process.returncode==0 and owner.exited()
    assert store.current()['sequence']==46 and store.current()['cohort']==str(new)
    assert CohortStore(QA/'installation',context,context_hash).current()==store.current()
    fresh=wait_wallet(lifecycle.new_ui,55)
    assert Path(fresh['pgn_update_data_root']).resolve()==data and sha(sentinel)==initial
    assert fresh['pgn_wallet_embedded_pid']>0 and fresh['pgn_wallet_embedded_pid']!=old_core_pid
    assert rpc(wallet_data,wallet_port,'getblockhash',[legacy['canonical_height']])==legacy['canonical_tip']
    save('ACTIVATION-PASS.json',dict(PASS=True,sequence=46,selector_reopens=True,old_ui_closed=True,
        retained_address=True,retained_data=True,core_restarted=True,versions=versions,manifest_sha256=new_digest))
    rpc(wallet_data,wallet_port,'setnetworkactive',[True])
    deadline=time.monotonic()+140
    while time.monotonic()<deadline and not lifecycle.new_ui.exited():
        try:
            native=rpc(wallet_data,wallet_port,'getblockchaininfo',[],timeout=8)
            shown=owned_ipc(lifecycle.new_ui,'--inspect-owned')
            height=min(int(shown['pgn_network_height']),native['blocks'])
            if height>legacy['canonical_height']:
                assert sha(sentinel)==initial and rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
                save('RESULT.json',dict(PASS=True,sequence=46,previous_height=legacy['canonical_height'],height=height,
                    real_native_restart=True,selector_retained=True,retained_wallet=True,bundle_transfers=1,versions=versions))
                break
        except (OSError,ValueError,RuntimeError):
            pass
        time.sleep(1)
    else:raise RuntimeError('Signed new install did not catch up with testnet')
finally:
    try:
        close_gui(lifecycle.new_ui if lifecycle and lifecycle.new_ui else owner)
    finally:
        if lifecycle:lifecycle.close()
        owner.close()
