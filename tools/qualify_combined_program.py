"""Verify the composed Atlas and engine host in the same Windows application directory."""
from pathlib import Path, PurePosixPath
import hashlib,http.client,json,os,re,subprocess,sys,zipfile,time,urllib.request,threading
from shutil import copy2,copytree
from contextlib import closing,contextmanager
assert os.environ.get('GITHUB_ACTIONS')=='true' and os.environ.get('RUNNER_ENVIRONMENT')=='github-hosted' and os.name=='nt'
ROOT=Path.cwd();QA=ROOT/'ui181-combined-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='39d3e5b4d9a4c9a24937be699a309a83fea8e5eecf1fde23a3b3947fddeef6ee'
archive=ROOT/'portable-ui181/PGN-Windows-181.zip'
assert archive.is_file()
def sha(p):
    with Path(p).open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(archive)==BASE_SHA
with zipfile.ZipFile(archive)as z:
    names=z.namelist()
    assert len(names)==426 and names.count('SOURCE-COMMIT.json')==1
    assert all(x.startswith('PGN/') or x=='SOURCE-COMMIT.json' for x in names)
    provenance=json.loads(z.read('SOURCE-COMMIT.json'))
    assert provenance['source_commit']=='e95dc5789a072de0c84fddf2999bf19597ab26f0'
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
with (wallet_data/'pgn.conf').open('a',encoding='utf-8') as f:f.write('networkactive=0\n')
wallet_port=json.loads((base/'Core/core-template.json').read_bytes())['deployment']['rpc_port']


def compose_program():
    plan_folder=ROOT/'qualification/ui181-combined'
    plan=json.loads((plan_folder/'composition.json').read_bytes())
    original_manifest=base/'components.json'
    assert sha(original_manifest)==plan['base_manifest_sha256']
    revision=(ROOT/'host91-qualified/SOURCE.txt').read_text(encoding='utf8').strip()
    provenance=json.loads((ROOT/'host91-qualified/ENGINE-HOST-SOURCE.json').read_bytes())
    host=ROOT/'host91-qualified/dist/horizon-engine-host'
    assert revision=='c6e5f0e1796081eacb68af256cf98e0f48405333'
    assert provenance['component_version']=='2026.11.18.91'
    assert (ROOT/'host91-qualified/HTTP-FROZEN-PASS.json').is_file()
    m,original,_,_=inventory(base)
    archive_target={x.relative_to(host).as_posix():x for x in host.rglob('*') if x.is_file()}
    host_prefix='EngineRuntime/network/'
    installed={name.removeprefix(host_prefix.casefold()) for name in original if name.startswith(host_prefix.casefold())}
    assert installed=={k.casefold() for k in archive_target}|{'bootstrap.json'}
    assert len(archive_target)==69 and sha(exe)==plan['exe_sha256']
    changed={}
    for rel,source in archive_target.items():
        path=host_prefix+rel
        target=base/path
        old_hash,new_hash=sha(target),sha(source)
        pair=plan['host'].get(path)
        if pair is None:
            assert new_hash==old_hash, path
            continue
        assert old_hash==pair['previous_sha256'] and new_hash==pair['sha256']
        assert source.stat().st_size==pair['bytes']
        copy2(source,target)
        changed[path]=new_hash
    assert set(changed)==set(plan['host'])
    asset=base/'assets/audit-animation.rcc'
    patch=plan_folder/'atlas-delta.patch'
    assert sha(asset)==plan['asset']['before'] and sha(patch)==plan['asset']['patch_sha']
    patch_env={**os.environ,'GIT_CEILING_DIRECTORIES':str(ROOT)}
    subprocess.run(['git','apply','--binary',str(patch.resolve())],cwd=base,env=patch_env,check=True)
    assert sha(asset)==plan['asset']['after']
    changed['assets/audit-animation.rcc']=sha(asset)
    prefixes=dict(ui='',gpu='EngineRuntime/',core='Core/',cpu='EngineRuntime/cpu/')
    seen=set()
    for component in m['components']:
        if component['id']=='gpu':
            component['version']=provenance['component_version']
        # Keep the UI version of the exact binary, as this is an unsigned fixture.
        for item in component['files']:
            path=prefixes[component['id']]+item['path']
            if path in changed:
                seen.add(path)
                item.update(bytes=(base/path).stat().st_size,sha256=changed[path])
    assert seen==set(changed)
    original_manifest.write_text(json.dumps(m,indent=2)+'\n',encoding='utf8')
    current,files,digest,_=inventory(base)
    assert len(files)==len(original)==424
    assert {k for k in files if files[k]['sha256']!=original[k]['sha256']}=={k.casefold() for k in changed}
    assert {c['id']:c['version'] for c in current['components']}==dict(ui='2026.11.18.181',gpu='2026.11.18.91',
        cpu='2026.11.6.389',core='2026.11.17.31')
    isolated_host=base/'EngineRuntime/network/horizon-engine-host.exe'
    assert sha(isolated_host)==provenance['executable_sha256']
    version=subprocess.check_output([str(isolated_host),'--version'],cwd=base,timeout=30,text=True).strip()
    test=json.loads(subprocess.check_output([str(isolated_host),'--audit-batch-self-test'],cwd=base,timeout=60))
    assert '2026.11.18.91' in version and test['passed'] and test['checks']==17
    assert test['no_child_processes'] and test['no_network'] and not test['cpu_activated']
    save('COMPOSED-FILES.json',dict(PASS=True,files=len(files),changes=changed,atlas_sha256=sha(asset),
        frozen_host=test['component_version'],host_checks=test['checks'],inventory_sha256=digest,source=plan['source']))
    return files


def show_community(owner):
    run=subprocess.run([str(owner.executable),'--show-owned',str(owner.pid),'Community'],capture_output=True,timeout=15)
    assert run.returncode==0
    receipt=json.loads(run.stdout)
    assert receipt['section']=='Community' and receipt['window_visible'] and not receipt['mining_control_changed']
    capture=Path(receipt['capture']).resolve()
    assert capture.is_relative_to((base/'evidence').resolve())
    until=time.monotonic()+8
    while not capture.exists() and time.monotonic()<until:
        time.sleep(.2)
    assert capture.exists() and capture.stat().st_size>4096
    with capture.open('rb') as stream:
        assert stream.read(8)==b'\x89PNG\r\n\x1a\n'
    copy2(capture,E/'Horizon-Network-Work.png')
    return {'section':receipt['section'],'window_visible':receipt['window_visible'],'mining_control_changed':receipt['mining_control_changed']}


files=compose_program()
for key in ('QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH'):
    os.environ.pop(key,None)
os.environ['WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS']='--use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader'
(base/'evidence').mkdir()
with (E/'combined.log').open('wb') as out:
    process=subprocess.Popen([str(exe),'--testnet'],cwd=base,
        stdout=out,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
owner=OwnedWindowsProcess(process.pid,exe,files['pgn-horizon.exe']['sha256'])
try:
    opening=wait_wallet(owner,100)
    assert Path(opening['pgn_update_application_root']).resolve()==base
    assert not Path(opening['pgn_update_data_root']).resolve().is_relative_to(base)
    assert opening['pgn_update_genesis']==GENESIS
    chain=rpc(wallet_data,wallet_port,'getblockhash',[legacy['canonical_height']])
    assert chain==legacy['canonical_tip']
    assert rpc(wallet_data,wallet_port,'getnetworkinfo',[])['networkactive'] is False
    show_community(owner)
    time.sleep(2.4)
    old_capture=base/'evidence'/f'owned-Community-{owner.pid}.png'
    old_capture.unlink()
    view=show_community(owner)
    last=owned_ipc(owner,'--inspect-owned')
    assert last['pgn_core_link_state']=='ready' and last['pgn_wallet_embedded_pid']==opening['pgn_wallet_embedded_pid']
    assert not last.get('pgn_network_desired_on')
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
    save('COMBINED-STARTUP-PASS.json',dict(PASS=True,real_windows_gui=True,host_version='2026.11.18.91',
        source_bounds='ordinary Windows application startup and Network Work opening; anchor flight tested separately on the same exact resource',
        same_wallet=True,same_chain=True,same_core=True,ui_section=view,no_mining=True,private=True))
finally:
    try:close_gui(owner)
    finally:owner.close()
