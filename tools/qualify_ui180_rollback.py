"""Actual public TUF download, cohort install and real Qt restart on isolated Windows."""
from pathlib import Path
import hashlib,http.client,json,os,re,subprocess,sys,zipfile,time,urllib.request,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from shutil import copytree
from contextlib import closing,contextmanager
from pathlib import PurePosixPath
from shutil import disk_usage
from urllib3.exceptions import ProtocolError
assert os.environ.get('GITHUB_ACTIONS')=='true' and os.environ.get('RUNNER_ENVIRONMENT')=='github-hosted' and os.name=='nt'
ROOT=Path.cwd();QA=ROOT/'ui180-rollback-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='627f3ce8879eea55b30c89fc4eb3959568e2c7f7e57d1b639fd259f5b0720c77'
URL='https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Windows-178.zip'
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

BUNDLE_SHA='08787d16e6159e0687a1310db57a551fda1a13730b42439893f7a81264c9cfc6'
ROOT_SHA='37f8978ea7e5844bb45b3f22c5cf2f51b68266f26a92583a4959a9e9a3870dbf'
UI_SHA='53987967a0caed084be4264da46d06ad0830c6b8f544c689b3e92d3193f5dccc'
CORE_SHA='3df925c084ad863521329fe438c804efa7d81905f7d350ad41981efa6402f25e'
draft=ROOT/'signed-candidate-48'
signed=draft/'metadata-unpacked'
signed.mkdir()
with zipfile.ZipFile(draft/'qa-catalogue48-metadata.zip')as z:
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
bundle=draft/(BUNDLE_SHA+'.components-48.zip')
assert bundle.stat().st_size==267879771 and sha(bundle)==BUNDLE_SHA
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
assert old_versions==dict(ui='2026.11.18.178',core='2026.11.17.31',gpu='2026.11.18.88',cpu='2026.11.6.389')
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
        platform='windows-x86_64',compatibility_set='pgn.epoch3.2026.11',minimum_sequence=46,
        installed=[dict(id=c['id'],abi='pgn.epoch3.2026.11',version=c['version'])for c in old['components']],
        protected_roots=[str(base),str(data)])).encode())
    fetcher=ExactDraftFetcher()
    client=AuthenticatedUpdates(QA/'downloads',root,ROOT_SHA,'https://windows-qualification.invalid/metadata/',
        'https://windows-qualification.invalid/targets/',context,context_hash,fetcher=fetcher)
    store=CohortStore.initialize(QA/'installation',base,context,context_hash)
    previous=store.current()
    assert previous['sequence']==46
    requested=['ui','gpu']
    assert client.refresh(requested)['sequence']==48
    candidate=store.prepare(client,requested,retain_download=True)
    assert fetcher.bundle_requests==1 and store.current()==previous and not owner.exited()
    assert client.refresh(requested)['sequence']==48
    repeat=store.prepare(client,requested)
    assert fetcher.bundle_requests==1 and repeat['cohort']==candidate['cohort']
    assert repeat['inventory_sha256']==candidate['inventory_sha256']
    new=Path(candidate['cohort'])
    manifest,declared,new_digest,_=inventory(new)
    versions={c['id']:c['version'] for c in manifest['components']}
    assert len(declared)==424 and new_digest=='dcd423d00a01b752fbb83adcb9bf267aece336f77d3a97592936e11fa60d890f'
    assert versions==dict(ui='2026.11.18.180',core='2026.11.17.31',gpu='2026.11.18.90',cpu='2026.11.6.389')
    assert sha(new/'PGN-Horizon.exe')==UI_SHA and sha(new/'Core/pgn-core.exe')==CORE_SHA
    command=lambda tool,*args:json.loads(subprocess.check_output([str(new/tool),*args],timeout=40))
    helper=command('UpdateRuntime/horizon-updater.exe','--version')
    host=command('EngineRuntime/network/horizon-engine-host.exe','--audit-batch-self-test')
    assert helper['version']=='2026.11.18.32' and host['passed'] and host['checks']==17
    save('PREPARE-PASS.json',dict(PASS=True,sequence=48,files=len(declared),manifest_sha256=new_digest,
        before_sequence=previous['sequence'],bundle_transfers=fetcher.bundle_requests,versions=versions))
    activation=dict(schema='PGN_WINDOWS_UPDATE_ACTIVATION_V1',
        ui_pid=owner.pid,ui_birth=owner.birth,application_root=state['pgn_update_application_root'],data_root=str(data))
    lifecycle=WindowsCohortLifecycle(store,base,activation)
    candidate_process=None
    candidate_state=None
    def reject_after_real_start(path):
        nonlocal_scratch['started']=lifecycle.start_and_verify(path)
        assert nonlocal_scratch['started']
        nonlocal_scratch['owner']=lifecycle.new_ui
        nonlocal_scratch['snapshot']=wait_wallet(lifecycle.new_ui,55)
        return False
    nonlocal_scratch={}
    assert store.activate(candidate,lifecycle.drain,reject_after_real_start,lifecycle.restore_previous) is False
    candidate_process=nonlocal_scratch['owner']
    candidate_state=nonlocal_scratch['snapshot']
    assert candidate_process.exited() and owner.exited() and lifecycle.new_ui.pid != owner.pid
    restored=wait_wallet(lifecycle.new_ui,55)
    assert store.current()==previous
    assert CohortStore(QA/'installation',context,context_hash).current()==previous
    assert Path(restored['pgn_update_application_root']).resolve()==base and lifecycle.new_ui.executable==exe
    assert Path(restored['pgn_update_data_root']).resolve()==data and sha(sentinel)==initial
    assert restored['pgn_wallet_embedded_pid']>0 and restored['pgn_wallet_embedded_pid']!=candidate_state['pgn_wallet_embedded_pid']
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
    assert rpc(wallet_data,wallet_port,'getblockhash',[legacy['canonical_height']])==legacy['canonical_tip']
    transaction=json.loads((store.root/'transaction.json').read_bytes())
    assert transaction['state']=='startup_failed' and transaction['restoration_completed'] is True
    save('ROLLBACK-PASS.json',dict(PASS=True,requested_sequence=48,restored_sequence=previous['sequence'],
        same_wallet=True,same_data_root=True,same_previous_executable=True,candidate_stopped=True,
        failure_injected_after_native_start=True,pointer_selected_previous=True,persistent_restoration_receipt=True,
        bundle_transfers=fetcher.bundle_requests,private=True))
    candidate_process.close()
finally:
    try:
        close_gui(lifecycle.new_ui if lifecycle and lifecycle.new_ui else owner)
    finally:
        if lifecycle:lifecycle.close()
        owner.close()
