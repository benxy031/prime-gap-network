"""Actual public TUF download, cohort install and real Qt restart on isolated Windows."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tarfile,zipfile,time,urllib.request
ROOT=Path.cwd();QA=ROOT/'ui176-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='bc66ef1e382f5bc5bfaf572237f4fcbfaf01ea829f018df752ec68b61ad4914b'
URL='https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Windows-175.zip?qualification=ui175core29'
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
    limit=time.monotonic()+80
    while state.get('pgn_core_link_state')!='ready':
        assert time.monotonic()<limit and not own.exited(),'Existing Core did not become ready'
        time.sleep(.35);state=owned_ipc(own,'--inspect-owned')
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
    client=AuthenticatedUpdates(QA/'downloads',root,root_hash,'https://raw.githubusercontent.com/wizz13150/prime-gap-network/qualification/ui176-recovery-updater/updates/testnet-epoch3/metadata/','https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-updates-epoch3/',context,context_hash)
    discovered=client.refresh(['ui']);assert discovered['publisher_authenticated'] and discovered['sequence']==42
    store=CohortStore.initialize(QA/'installation',base,context,context_hash)
    candidate=store.prepare(client,['ui'])
    new=Path(candidate['cohort']);manifest=inventory(new)[0]
    versions={c['id']:c['version']for c in manifest['components']}
    dependency_probe=subprocess.run([str(new/'Core/pgn-core.exe'),'-version'],capture_output=True,timeout=15)
    assert dependency_probe.returncode==0,dependency_probe.stderr.decode(errors='replace')[-1000:]
    assert versions==dict(ui='2026.11.17.176',core='2026.11.17.29',gpu='2026.11.17.85',cpu='2026.11.6.389')
    assert 'ui' in candidate['selected'] and set(candidate['selected'])<=set(versions)
    def unchanged(cohort):
        return {c['id']:(c['version'],{row['path']:(row['bytes'],row['sha256']) for row in c['files']})
                for c in cohort['components'] if c['id']!='ui'}
    assert unchanged(old)==unchanged(manifest)
    assert {c['id']:c['version']for c in old['components']}==dict(ui='2026.11.17.175',core='2026.11.17.29',gpu='2026.11.17.85',cpu='2026.11.6.389')
    assert sha(new/'PGN-Horizon.exe')=='811ced64f9189b5e984bc46b9ef2f4c9359865409da528d28a0bcc4261589f64'!=sha(exe)
    assert sha(new/'Core/pgn-core.exe')=='6e0d827229ab23fc36f0957f53e43548f6f05437d826b15e5ed98ea0825335ce'==sha(base/'Core/pgn-core.exe')
    host_probe=json.loads(subprocess.check_output([str(new/'EngineRuntime/network/horizon-engine-host.exe'),'--audit-batch-self-test'],timeout=45))
    assert host_probe['passed'] and host_probe['checks']==17 and host_probe['no_network']
    updater_probe=subprocess.run([str(new/'UpdateRuntime/horizon-updater.exe'),'--self-test'],capture_output=True,timeout=20)
    assert updater_probe.returncode==0 and json.loads(updater_probe.stdout)['version']=='2026.11.17.30'
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
    seen=[]
    for section in ('Wallet','Social','SocialMiners','Mining'):
        view=gui_command(life.new_ui,'--show-owned',section);seen.append(section)
        (E/(section+'.json')).write_text(json.dumps(view,indent=2)+'\n')
    new_pid=life.new_ui.pid;gui_command(life.new_ui,'--close-owned')
    for _ in range(80):
        if life.new_ui.exited():break
        time.sleep(.25)
    assert life.new_ui.exited(),'New real GUI failed to close normally'
    stop_fixture_core(new/'Core/pgn-core.exe',new_core_pid,wallet_data,wallet_port)
    current=store.current();reopened=CohortStore(QA/'installation',context,context_hash)
    assert reopened.current()==current and sha(sentinel)==saved
    result=dict(PASS=True,requested_components=['ui'],selected_with_dependencies=candidate['selected'],public_updater=True,fixed_public_baseline=True,metadata_sequence=42,old_versions={c['id']:c['version']for c in old['components']},new_versions=versions,old_normal_exit=process.returncode,real_Qt_restart=True,new_pid=new_pid,new_normal_close=True,selector_retained=True,isolated_data_bytes_preserved=True,existing_wallet_address_preserved=True,active_core_restarted=True,within_previous_health_budget=observed_launch[0],verified_new_helper_version='2026.11.17.30',sections=seen,seconds=time.monotonic()-start,scope='Standard per-user Windows layout: actual download/authentication/cohort activation/Qt restart from the public UI175/Core29/GPU85 using the normal Interface-only selection. The selected UI hash is exact and the existing synthetic wallet/address reopens through the selected Core.')
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
(QA/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
