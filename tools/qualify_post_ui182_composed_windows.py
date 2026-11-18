"""Boot the development overlay of the qualified Windows archive."""
from pathlib import Path, PurePosixPath
import hashlib,http.client,json,os,re,subprocess,sys,zipfile,time,urllib.request,threading
from contextlib import closing,contextmanager
from shutil import copy2,copytree
assert os.environ.get('GITHUB_ACTIONS')=='true' and os.environ.get('RUNNER_ENVIRONMENT')=='github-hosted' and os.name=='nt'
ROOT=Path.cwd();QA=ROOT/'ui182-portable-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='2d2fcdfd1e5707492809591532d87250fbf2770f0e3fd4ce70fd66d0ef15e871'
archive=ROOT/'portable-ui182/PGN-Windows-182.zip'
assert archive.is_file()
def sha(p):
    with Path(p).open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(archive)==BASE_SHA
with zipfile.ZipFile(archive)as z:
    names=z.namelist()
    assert len(names)==426 and names.count('SOURCE-COMMIT.json')==1
    assert all(x.startswith('PGN/') or x=='SOURCE-COMMIT.json' for x in names)
    provenance=json.loads(z.read('SOURCE-COMMIT.json'))
    assert provenance['source_commit']=='7af0b29ede822aa3d7b0ca8e91b37c89b9156e56'
    z.extractall(QA/'baseline')
manifests=list((QA/'baseline').rglob('components.json'));assert len(manifests)==1
base=manifests[0].parent.resolve();exe=base/'PGN-Horizon.exe'
assert base.name=='PGN' and not (base/'SOURCE-COMMIT.json').exists(), 'Publisher provenance must remain outside code'
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'src/update'),str(ROOT/'tests')]
from horizon_component_install import inventory
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
with (wallet_data/'pgn.conf').open('a',encoding='utf-8') as f:f.write('networkactive=0\nrpcthreads=1\n')
wallet_port=json.loads((base/'Core/core-template.json').read_bytes())['deployment']['rpc_port']

manifest,files,digest,_=inventory(base)
versions={c['id']:c['version'] for c in manifest['components']}
assert versions==dict(ui='2026.11.18.182', core='2026.11.17.31', gpu='2026.11.18.91', cpu='2026.11.6.389')
assert len(files)==424
assert files['pgn-horizon.exe']['sha256']=='2bb34ae82100c932ede11d3519c376113379956cca599335e1fd6d49b3894c98'
def compose_program():
    assert sha(exe)==files['pgn-horizon.exe']['sha256']
    assert (ROOT/'build-app/QT_VERSION.txt').read_text().strip()=='6.12.0'
    provenance=json.loads((ROOT/'host-build/ENGINE-HOST-SOURCE.json').read_bytes())
    assert provenance['sources']['src/engine/runtime_host/horizon_network_audits.py']==sha(ROOT/'src/engine/runtime_host/horizon_network_audits.py')
    ui=ROOT/'build-app/PGN-Horizon.exe'
    assert ui.is_file() and sha(ui)!=sha(exe)
    copy2(ui,exe)
    original_host=base/'EngineRuntime/network'
    assert original_host.resolve().parent == (base/'EngineRuntime').resolve()
    assert original_host.relative_to(base).as_posix()=='EngineRuntime/network'
    retained=QA/'donor-host'
    original_host.rename(retained)
    copytree(ROOT/'host-build/dist/horizon-engine-host',original_host)
    copy2(retained/'bootstrap.json',original_host/'bootstrap.json')
    host=original_host/'horizon-engine-host.exe'
    assert sha(host)==provenance['executable_sha256']==sha(ROOT/'host-build/dist/horizon-engine-host/horizon-engine-host.exe')
    for component in manifest['components']:
        if component['id']=='ui':
            row=next(x for x in component['files'] if x['path']=='PGN-Horizon.exe')
            row.update(bytes=exe.stat().st_size,sha256=sha(exe))
        if component['id']=='gpu':
            component['files']=[row for row in component['files'] if not row['path'].startswith('network/')]
            for file in sorted(original_host.rglob('*')):
                if not file.is_file():
                    continue
                rel=file.relative_to(base/'EngineRuntime').as_posix()
                component['files'].append(dict(path=rel,bytes=file.stat().st_size,sha256=sha(file)))
    (base/'components.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8')
    _,verified,inventory_digest,_=inventory(base)
    touched=lambda key:key=='pgn-horizon.exe' or key.startswith('engineruntime/network/')
    assert ({key:value for key,value in files.items() if not touched(key)}
            == {key:value for key,value in verified.items() if not touched(key)})
    test=json.loads(subprocess.check_output([str(host),'--audit-batch-self-test'],cwd=base,timeout=60))
    assert test['passed'] and test['checks']==17 and test['no_network'] and test['no_child_processes']
    commit=subprocess.check_output(['git','rev-parse','HEAD^'],cwd=ROOT,text=True).strip()
    assert commit=="d740405addcd6e65fe933d417019542ef0e68ec3"
    save('COMPOSED-FILES.json',dict(PASS=True,development_source=commit,donor_sha256=BASE_SHA,
        ui_sha256=sha(exe),host_sha256=sha(host),native_qprocess_test=True,host_checks=test['checks'],
        inventory_sha256=inventory_digest,authenticated_release=False))
    return verified


files=compose_program()
for key in ('QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH'):
    os.environ.pop(key,None)
with (E/'standalone.log').open('wb') as out:
    process=subprocess.Popen([str(exe),'--testnet'],cwd=base,
        stdout=out,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
owner=OwnedWindowsProcess(process.pid,exe,files['pgn-horizon.exe']['sha256'])
try:
    state=wait_wallet(owner,100)
    assert Path(state['pgn_update_application_root']).resolve()==base
    assert not Path(state['pgn_update_data_root']).resolve().is_relative_to(base)
    assert state['pgn_update_genesis']==GENESIS
    assert not state.get('pgn_update_startup_notice_visible')
    assert rpc(wallet_data,wallet_port,'getblockhash',[legacy['canonical_height']])==legacy['canonical_tip']
    first_core=state['pgn_wallet_embedded_pid']
    deadline=time.monotonic()+35
    while time.monotonic()<deadline:
        shown=owned_ipc(owner,'--inspect-owned')
        if shown['pgn_global_network_status'].endswith('NO PEERS'):
            assert shown['pgn_wallet_embedded_pid']==first_core
            break
        time.sleep(.4)
    else:raise AssertionError('Core ready with zero peers but no waiting status appeared')
    save('STARTUP-PASS.json',dict(PASS=True,ui=versions['ui'],archive=BASE_SHA,retained_wallet=True,
        restored_at=legacy['canonical_height'],expected_genesis=GENESIS,portable_directory='PGN',no_input=True))
    receipt=subprocess.run([str(exe),'--show-owned',str(owner.pid),'Network'],capture_output=True,timeout=15)
    assert receipt.returncode==0
    view=json.loads(receipt.stdout)
    assert not view.get('error') and view.get('section')=='Network'
    last=owned_ipc(owner,'--inspect-owned')
    assert last['pgn_core_link_state']=='ready' and last['pgn_wallet_embedded_pid']==first_core
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine']
    save('COMPOSED-STARTUP-PASS.json',dict(PASS=True,verified_actual_startup=True,view='Network',
        preserved_wallet=True,preserved_chain_height=legacy['canonical_height'],same_core_after_navigation=True,
        isolated=True,no_mining=True,ui_sha256=sha(exe)))

finally:
    try:close_gui(owner)
    finally:owner.close()
