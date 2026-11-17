"""Actual public TUF download, cohort install and real Qt restart on isolated Windows."""
from pathlib import Path
import hashlib,http.client,json,os,subprocess,sys,zipfile,time,urllib.request,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from shutil import copytree
from contextlib import closing,contextmanager
from pathlib import PurePosixPath
from shutil import disk_usage
from urllib3.exceptions import ProtocolError
ROOT=Path.cwd();QA=ROOT/'ui177-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='28e754c491daca24673f87f04d0be5a9c1b64883bec830e58e702e556b4aeb69'
URL='https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Windows-176.zip?qualification=ui177resilience'
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
from native_grouped249_actual import Node
from qualify_portable_slow_core_windows import rpc,stop_fixture_core
WALLET='QA_Updater_Retained_Wallet'
roaming=(QA/'Roaming').resolve()
local=(QA/'Local').resolve()
roaming.mkdir();local.mkdir()
os.environ.update(APPDATA=str(roaming),LOCALAPPDATA=str(local))
wallet_data=roaming/'PGNCore'/'networks'/GENESIS[:16]
fixture=ROOT/'qualification/ui177/legacy-chain.zip'
assert sha(fixture)=='33f5d5a206786715725ffbed770bfeba3b014898bf6c0e734419fbbb62a050c6'
with zipfile.ZipFile(fixture) as old_chain:
    assert sum(info.file_size for info in old_chain.infolist())<128*1024*1024
    for info in old_chain.infolist():
        name=PurePosixPath(info.filename)
        assert not name.is_absolute() and '..' not in name.parts and 'wallets' not in name.parts
        assert chr(92) not in info.filename and ':' not in info.filename
        path=wallet_data/name
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(old_chain.read(info))
legacy=json.loads((wallet_data/'FIXTURE.json').read_bytes())
assert legacy['public_data_only'] and legacy['no_wallets']
assert legacy['genesis']==GENESIS and legacy['legacy_height']==4245 and legacy['canonical_height']==5681
for name,signature in legacy['files'].items():
    assert sha(wallet_data/name)==signature
node=Node(base/'Core/pgn-core.exe',wallet_data)
try:
    assert node.rpc('createwallet',[WALLET,False,False,'',False,True])['name']==WALLET
    address=node.rpc('getnewaddress',['Updater fixture','bech32'])
    assert node.rpc('getaddressinfo',[address])['ismine'] is True
finally:
    node.close()
assert (wallet_data/'testnet/wallets'/WALLET/'wallet.dat').is_file()
with (wallet_data/'pgn.conf').open('a',encoding='utf-8') as f:
    f.write('networkactive=0\n')
wallet_port=json.loads((base/'Core/core-template.json').read_bytes())['deployment']['rpc_port']
def gui_command(owned,operation,*args):
    assert not owned.exited()
    run=subprocess.run([str(owned.executable),operation,str(owned.pid),*args],capture_output=True,timeout=12)
    assert run.returncode==0,(operation,run.stderr.decode(errors='replace')[-500:])
    return None if operation=='--close-owned' else json.loads(run.stdout)
@contextmanager
def small_update_volume(root):
    # A capped disposable disk exercises the real operating-system free-space
    # path without changing the runner's own volumes or filling its system disk.
    assert os.environ.get('GITHUB_ACTIONS')=='true' and os.name=='nt'
    vhd=(root/'update-capacity.vhd').resolve()
    assert vhd.parent==root.resolve() and not vhd.exists()
    drive=next(f'{letter}:/' for letter in 'TUVXYZ' if not Path(f'{letter}:/').exists())
    volume=Path(drive)
    command=root/'update-capacity.diskpart.txt'
    command.write_text(f'create vdisk file="{vhd}" maximum=160 type=expandable\n'
                       f'select vdisk file="{vhd}"\nattach vdisk\ncreate partition primary\n'
                       f'format fs=ntfs quick label=PGNQA\nassign letter={drive[0]}\nexit\n',
                       encoding='ascii')
    try:
        created=subprocess.run(['diskpart.exe','/s',str(command)],capture_output=True,timeout=100)
        (E/'volume-setup.log').write_bytes(created.stdout+created.stderr)
        assert created.returncode==0 and vhd.is_file() and volume.exists(),'Low-space VHD unavailable'
        assert 8*1024*1024<disk_usage(volume).free<225*1024*1024
        yield volume
    finally:
        if vhd.is_file():
            command.write_text(f'select vdisk file="{vhd}"\ndetach vdisk\nexit\n',encoding='ascii')
            subprocess.run(['diskpart.exe','/s',str(command)],capture_output=True,timeout=40,check=True)

class CutTheSignedBundle(ResumableTargetFetcher):
    def __init__(self):
        super().__init__()
        self.requests=[]

    def fetch_target(self,url,offset,length):
        self.requests.append(offset)
        first=len(self.requests)==1
        with closing(super().fetch_target(url,offset,length)) as response:
            for chunk in response:
                yield chunk
                offset+=len(chunk)
                if first and offset>=8*1024*1024:
                    raise ProtocolError('Controlled end-to-end cut during the anonymous signed bundle transfer')

def verify_slow_core_handoff(current_code, expected, runtime_directory):
    """Run a real portable GUI against the same recorded older owned process."""
    profile_path=(Path(runtime_directory)/'wallet-core.json').resolve(strict=True)
    assert os.environ.get('GITHUB_ACTIONS')=='true'
    original=profile_path.read_bytes()
    profile=json.loads(original)
    assert Path(profile['data_directory']).resolve()==wallet_data.resolve()
    # Qt's Windows known-folder lookup can ignore the two environment aliases.
    # The hosted runner is disposable, and the actual profile must bind this
    # test's synthetic data before its known app-local file may be exercised.
    assert profile.get('genesis_hash')==GENESIS and profile.get('wallet')==WALLET
    if not profile_path.is_relative_to(QA.resolve()):
        assert profile_path.parent.name==GENESIS and profile_path.parent.parent.name=='testnet'
    owner_path=profile_path.with_name('wallet-owned.json')
    prior=QA/'earlier-owned'
    if not prior.exists():
        copytree(base/'Core',prior/'Core')
    previous_exe=(prior/'Core/pgn-core.exe').resolve(strict=True)
    previous=Node(previous_exe,wallet_data)
    previous_owner=OwnedWindowsProcess(previous.process.pid,previous_exe,sha(previous_exe))
    assert rpc(wallet_data,previous.rpcport,'getblockhash',[0])==GENESIS
    stops=[]
    class Exchange(BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):
            pass
        def do_POST(self):
            length=int(self.headers['Content-Length'])
            assert 0<length<262144
            body=self.rfile.read(length)
            query=json.loads(body)
            if query['method']=='stop':
                stops.append(time.monotonic())
                if len(stops)==1:
                    # Drop the original shutdown request without delivering it.
                    # The old request budget expires before the controlled loss.
                    time.sleep(5)
                    try:
                        self.send_response(503)
                        self.send_header('Content-Length','0')
                        self.end_headers()
                    except (OSError,ConnectionError):
                        pass
                    self.close_connection=True
                    return
                self.server.shutdown()
                self.server.server_close()
            conn=http.client.HTTPConnection('127.0.0.1',previous.rpcport,timeout=20)
            try:
                conn.request('POST',self.path,body,{'Authorization':self.headers['Authorization'],
                         'Content-Type':'application/json'})
                response=conn.getresponse()
                payload=response.read(2*1024*1024)
                self.send_response(response.status)
                self.send_header('Content-Type','application/json')
                self.send_header('Content-Length',str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError,ConnectionResetError):
                pass
            finally:
                conn.close()
    gate=ThreadingHTTPServer(('127.0.0.1',0),Exchange)
    bridge=threading.Thread(target=gate.serve_forever,args=(.025,),daemon=True)
    bridge.start()
    new_port=gate.server_port
    profile.update(executable=previous_exe.as_posix(),executable_sha256=sha(previous_exe),rpc_port=new_port)
    binding={key:profile[key] for key in ('executable','executable_sha256','data_directory','genesis_hash','rpc_port','peer_port','chain','frontend','wallet')}
    binding.update(pid=str(previous_owner.pid),birth=previous_owner.birth,
                   schema='pgn-core-owned-3' if profile['schema']=='pgn-core-deployment-3' else 'pgn-core-owned-2')
    if binding['schema']=='pgn-core-owned-3':
        binding['approved_data_directory']=profile['approved_data_directory']
    def qt_path(text):
        return Path(text).resolve(strict=True).as_posix()
    binding['data_directory']=qt_path(binding['data_directory'])
    if 'approved_data_directory' in binding:
        binding['approved_data_directory']=qt_path(binding['approved_data_directory'])
    profile_path.write_text(json.dumps(profile),encoding='utf-8')
    owner_path.write_text(json.dumps(binding),encoding='utf-8')
    running=None;app=None;accepted=None;observed_stops=None
    try:
        code=current_code/'PGN-Horizon.exe'
        running=subprocess.Popen([str(code),'--testnet','--start-minimized'],cwd=current_code,creationflags=0x08000000)
        app=OwnedWindowsProcess(running.pid,code,sha(code))
        deadline=time.monotonic()+80
        while time.monotonic()<deadline and not app.exited():
            try:
                visible=owned_ipc(app,'--inspect-owned')
                oid=visible.get('pgn_wallet_embedded_pid')
                if visible.get('pgn_core_link_state')=='ready' and stops and oid:
                    if expected and oid!=previous_owner.pid:
                        assert len(stops)==2 and previous_owner.exited()
                        assert rpc(wallet_data,new_port,'getaddressinfo',[address])['ismine'] is True
                        accepted=oid
                        break
                    if not expected and oid==previous_owner.pid:
                        assert len(stops)==1 and not previous_owner.exited()
                        accepted=oid
                        break
            except (OSError,ValueError,RuntimeError):
                pass
            time.sleep(.25)
        assert accepted is not None,dict(expected=expected,stops=len(stops))
        observed_stops=len(stops)
    finally:
        if app and not app.exited():
            gui_command(app,'--close-owned')
            running.wait(timeout=30)
        if app:
            app.close()
        if expected and accepted and accepted!=previous_owner.pid:
            stop_fixture_core(current_code/'Core/pgn-core.exe',accepted,wallet_data,new_port)
        if bridge.is_alive():
            gate.shutdown()
        gate.server_close();bridge.join(timeout=5)
        previous.close();previous_owner.close()
        profile_path.write_bytes(original)
    return dict(expected_handoff=expected,old_core_exited=expected,shutdown_requests=observed_stops)

def baseline_progress(snapshot):
    stable_names=('pgn_core_link_state','pgn_core_link_detail','pgn_wallet_embedded_pid','pgn_network_height',
                  'pgn_wallet_frontend_loaded','pgn_wallet_setup_required','pgn_core_link_transitions')
    core={name:snapshot[name] for name in stable_names if name in snapshot}
    core['core_note']=str(core.get('pgn_core_link_detail',''))[:250]
    log=wallet_data/'testnet/debug.log'
    if log.is_file():
        with log.open('rb') as file:
            file.seek(max(0,log.stat().st_size-16384))
            rows=file.read(16384).decode(errors='replace').splitlines()
        useful=[row for row in rows if any(marker in row.lower() for marker in
                ('updateTip: new best'.lower(),'error','init message','shutdown','revalidating','loaded best'))]
        core['native_tail']=useful[-10:]
        import re
        heights=[int(match.group(1)) for row in useful if (match:=re.search(r'height=(\d+)',row))]
        core['native_height']=max(heights,default=0)
    return core
old,files,_,_=inventory(base)
for name in ('config','data','cache','runtime'):(QA/name).mkdir()
for name in ('QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH'):os.environ.pop(name,None)
start=time.monotonic();process=None;life=None;own=None;result=None
out=(E/'stdout.log').open('wb');err=(E/'stderr.log').open('wb')
try:
    process=subprocess.Popen([str(exe),'--testnet','--start-minimized','--update-healthcheck'],cwd=base,stdout=out,stderr=err,creationflags=0x08000000)
    own=OwnedWindowsProcess(process.pid,exe,files['pgn-horizon.exe']['sha256'])
    state=None
    for _ in range(240):
        assert process.poll()is None,'Old real GUI exited'
        try:state=owned_ipc(own,'--inspect-owned');break
        except (RuntimeError,OSError,ValueError):time.sleep(.25)
    assert state and state['pgn_update_genesis']==GENESIS
    first=time.monotonic()
    last_progress=first
    highest=0
    while state.get('pgn_core_link_state')!='ready':
        snapshot=baseline_progress(state)
        (E/'BASELINE-READINESS.json').write_text(json.dumps(snapshot,indent=2)+'\n',encoding='utf-8')
        height=snapshot.get('native_height',0)
        if height>highest:
            highest=height
            last_progress=time.monotonic()
        now=time.monotonic()
        assert now-last_progress<80 and now-first<200 and not own.exited(), {
            'cause':'baseline old Core readiness failed', 'state':snapshot.get('pgn_core_link_state'),
            'detail':snapshot['core_note'], 'native_height':highest,
        }
        time.sleep(1);state=owned_ipc(own,'--inspect-owned')
    assert rpc(wallet_data,wallet_port,'listwallets',[])==[WALLET]
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
    old_core_pid=state['pgn_wallet_embedded_pid']
    assert old_core_pid>0
    data=Path(state['pgn_update_data_root']);assert not data.resolve().is_relative_to(base), 'Standard Windows data must be outside its executable cohort'
    sentinel=data/'wallet-preservation-sentinel.dat';sentinel.parent.mkdir(parents=True,exist_ok=True);sentinel.write_bytes(b'ISOLATED TEST: KEEP EXACT BYTES')
    saved=sha(sentinel)
    context,context_hash=stage.context_from_bytes(json.dumps(dict(schema='PGN_COMPONENT_STAGE_CONTEXT_V1',network='testnet',genesis_hash=GENESIS,platform='windows-x86_64',compatibility_set='pgn.epoch3.2026.11',minimum_sequence=1,installed=[],protected_roots=[str(base),str(data)])).encode())
    root=(ROOT/'updates/testnet-epoch3/metadata/root.json').read_bytes();root_hash=hashlib.sha256(root).hexdigest()
    assert root_hash=='37f8978ea7e5844bb45b3f22c5cf2f51b68266f26a92583a4959a9e9a3870dbf'
    metadata='https://raw.githubusercontent.com/wizz13150/prime-gap-network/qualification/ui177-complete-windows/updates/testnet-epoch3/metadata/'
    targets='https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-updates-epoch3/'
    components=['ui','gpu']
    store=CohortStore.initialize(QA/'installation',base,context,context_hash)
    previous_selection=store.current()
    with small_update_volume(QA) as small:
        constrained=AuthenticatedUpdates(small/'updates',root,root_hash,metadata,targets,context,context_hash)
        assert constrained.refresh(components)['publisher_authenticated']
        try:
            store.prepare(constrained,components)
        except stage.StageError as error:
            assert 'Insufficient download disk space' in str(error),str(error)
        else:
            raise AssertionError('Download admitted despite insufficient real disk space')
        assert store.current()==previous_selection and not own.exited() and sha(sentinel)==saved
        assert owned_ipc(own,'--inspect-owned')['pgn_wallet_embedded_pid']==old_core_pid
    fetcher=CutTheSignedBundle()
    client=AuthenticatedUpdates(QA/'downloads',root,root_hash,metadata,targets,context,context_hash,fetcher=fetcher)
    discovered=client.refresh(components)
    assert discovered['publisher_authenticated'] and discovered['sequence']==43
    candidate=store.prepare(client,components)
    assert len(fetcher.requests)==2 and fetcher.requests[0]==0 and fetcher.requests[1]>=8*1024*1024,fetcher.requests
    new=Path(candidate['cohort']);manifest=inventory(new)[0]
    versions={c['id']:c['version']for c in manifest['components']}
    dependency_probe=subprocess.run([str(new/'Core/pgn-core.exe'),'-version'],capture_output=True,timeout=15)
    assert dependency_probe.returncode==0,dependency_probe.stderr.decode(errors='replace')[-1000:]
    assert versions==dict(ui='2026.11.17.177',core='2026.11.17.29',gpu='2026.11.17.87',cpu='2026.11.6.389')
    assert 'ui' in candidate['selected'] and set(candidate['selected'])<=set(versions)
    def unchanged(cohort):
        return {c['id']:(c['version'],{row['path']:(row['bytes'],row['sha256']) for row in c['files']})
                for c in cohort['components'] if c['id'] in ('core','cpu')}
    assert unchanged(old)==unchanged(manifest)
    assert {c['id']:c['version']for c in old['components']}==dict(ui='2026.11.17.176',core='2026.11.17.29',gpu='2026.11.17.85',cpu='2026.11.6.389')
    assert sha(new/'PGN-Horizon.exe')=='0fa1ccec5011ca2e52bb4b5fc5b03af119e328ed98faf37cb247234d8ec19b46'!=sha(exe)
    assert sha(new/'Core/pgn-core.exe')=='6e0d827229ab23fc36f0957f53e43548f6f05437d826b15e5ed98ea0825335ce'==sha(base/'Core/pgn-core.exe')
    host_probe=json.loads(subprocess.check_output([str(new/'EngineRuntime/network/horizon-engine-host.exe'),'--audit-batch-self-test'],timeout=45))
    assert host_probe['passed'] and host_probe['checks']==17 and host_probe['no_network']
    updater_probe=subprocess.run([str(new/'UpdateRuntime/horizon-updater.exe'),'--self-test'],capture_output=True,timeout=20)
    assert updater_probe.returncode==0 and json.loads(updater_probe.stdout)['version']=='2026.11.17.31'
    activation=dict(schema='PGN_WINDOWS_UPDATE_ACTIVATION_V1',ui_pid=own.pid,ui_birth=own.birth,application_root=state['pgn_update_application_root'],data_root=str(data))
    life=WindowsCohortLifecycle(store,base,activation)
    observed_launch=[]
    original_launch=life.launch
    def measure_launch(path):
        tick=time.monotonic()
        accepted=original_launch(path)
        if Path(path)==new:
            observed_launch.append(time.monotonic()-tick)
        return accepted
    life.launch=measure_launch
    assert store.activate(candidate,life.drain,life.start_and_verify,life.restore_previous)
    assert len(observed_launch)==1 and observed_launch[0]<60.0, observed_launch
    process.wait(timeout=20);assert process.returncode==0 and life.ui.exited()
    assert store.current()['cohort']==str(new)
    live=owned_ipc(life.new_ui,'--inspect-owned')
    (E/'before.json').write_text(json.dumps(state,indent=2)+'\n')
    (E/'after.json').write_text(json.dumps(live,indent=2)+'\n')
    assert live['pgn_update_genesis']==GENESIS and Path(live['pgn_update_data_root']).resolve()==data.resolve(), 'Updated Windows data-root identity differs'
    assert sha(sentinel)==saved
    assert live['pgn_core_link_state']=='ready'
    assert rpc(wallet_data,wallet_port,'listwallets',[])==[WALLET]
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine'] is True
    new_core_pid=live['pgn_wallet_embedded_pid']
    assert new_core_pid>0 and new_core_pid!=old_core_pid
    try:
        prior=OwnedWindowsProcess(old_core_pid,base/'Core/pgn-core.exe',sha(base/'Core/pgn-core.exe'))
    except ProcessAbsentError:
        prior=None
    if prior is not None:
        try:assert prior.exited(),'Previous Core remains running'
        finally:prior.close()
    rpc(wallet_data,wallet_port,'setnetworkactive',[True])
    accepted_height=0
    catchup_deadline=time.monotonic()+140
    while time.monotonic()<catchup_deadline:
        try:
            current_chain=rpc(wallet_data,wallet_port,'getblockchaininfo',[],timeout=8)
            ui=owned_ipc(life.new_ui,'--inspect-owned')
            if (current_chain['blocks']>legacy['canonical_height']
                    and int(ui['pgn_network_height'])>legacy['canonical_height']):
                assert rpc(wallet_data,wallet_port,'getblockhash',[4246])=='8f10d820e15956f939c738a53e13d02685fb606284f2c33fb0414aa8970321a1'
                accepted_height=current_chain['blocks']
                break
        except (OSError,ValueError,RuntimeError):
            pass
        time.sleep(1)
    assert accepted_height>legacy['canonical_height'],'GUI/Core did not validate beyond the old cached chain'
    seen=[]
    for section in ('Wallet','Social','SocialMiners','Mining'):
        view=gui_command(life.new_ui,'--show-owned',section);seen.append(section)
        (E/(section+'.json')).write_text(json.dumps(view,indent=2)+'\n')
    for action in ('DiagnosticsStart','DiagnosticsStop','DiagnosticsExport'):
        gui_command(life.new_ui,'--show-owned',action)
    diagnostic_name=f'diagnostic-{life.new_ui.pid}.zip'
    locations=[new/'evidence'/diagnostic_name,Path(live['pgn_update_application_root'])/'evidence'/diagnostic_name]
    paths=list(dict.fromkeys(path.resolve() for path in locations))
    assert all(path.is_relative_to(QA.resolve()) for path in paths)
    until=time.monotonic()+25
    diagnostic=None
    while time.monotonic()<until:
        diagnostic=next((path for path in paths if path.is_file()),None)
        if diagnostic is not None:break
        time.sleep(.25)
    assert diagnostic is not None,'Built-in diagnostic export did not produce a ZIP'
    with zipfile.ZipFile(diagnostic) as bundle:
        entry=bundle.getinfo('diagnostics.json')
        assert entry.file_size<=2*1024*1024
        context=json.loads(bundle.read(entry))['context']
    assert [event['stage'] for event in context['startup']]==[1,2,3,4,5,6]
    assert context['core_first'] and any(row['method']=='getblockchaininfo' for row in context['core_rpc_history'])
    assert context['installation']['running_program']['versions']['ui']=='2026.11.17.177'
    assert context['installation']['core']['observed_process_matches_owner']
    assert 'local_paths' not in context
    new_pid=life.new_ui.pid;gui_command(life.new_ui,'--close-owned')
    for _ in range(80):
        if life.new_ui.exited():break
        time.sleep(.25)
    assert life.new_ui.exited(),'New real GUI failed to close normally'
    stop_fixture_core(new/'Core/pgn-core.exe',new_core_pid,wallet_data,wallet_port)
    current=store.current();reopened=CohortStore(QA/'installation',context,context_hash)
    assert reopened.current()==current and sha(sentinel)==saved
    result=dict(PASS=True,requested_components=components,selected_with_dependencies=candidate['selected'],
        metadata_sequence=43,old_versions={c['id']:c['version'] for c in old['components']},
        new_versions=versions,old_normal_exit=process.returncode,real_Qt_restart=True,new_pid=new_pid,
        new_normal_close=True,selector_retained=True,existing_wallet_address_preserved=True,
        isolated_data_bytes_preserved=True,active_core_restarted=True,
        hardware_mining_exercised=False,host_unpack_self_test_passed=True,
        diagnostic_opened_after_startup=True,downloaded_resume_byte=fetcher.requests[1],explicit_insufficient_disk_error=True,
        old_application_survived_insufficient_space=True,legacy_height=legacy['legacy_height'],
        validated_live_height=accepted_height,verified_new_helper_version='2026.11.17.31',
        within_previous_health_budget=observed_launch[0],sections=seen,seconds=time.monotonic()-start)
finally:
    active=life.new_ui if life and life.new_ui else own
    if active and not active.exited():
        gui_command(active,'--close-owned')
        for _ in range(80):
            if active.exited():break
            time.sleep(.25)
        assert active.exited(),'Owned test GUI still running; no forced kill'
    if process is not None:process.wait(timeout=20)
    if life:life.close()
    if own:own.close()
    out.close();err.close()
assert result
result['previous_exe_timeout_negative_control']=verify_slow_core_handoff(base,False,live['pgn_update_core_runtime_directory'])
result['ordinary_portable_timeout_recovery']=verify_slow_core_handoff(new,True,live['pgn_update_core_runtime_directory'])
(QA/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
