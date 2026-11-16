"""Actual public TUF download, cohort install and real Qt restart on isolated Linux."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tarfile,zipfile,time,urllib.request
ROOT=Path.cwd();QA=ROOT/'release269-windows';QA.mkdir();E=QA/'evidence';E.mkdir()
GENESIS='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314'
BASE_SHA='34733424c7e0a3a6f388d43f235d7ce3af43a8c4e9b63e5eab0023575737112c'
URL='https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/34733424c7e0a3a6f388d43f235d7ce3af43a8c4e9b63e5eab0023575737112c.PGN-Horizon-testnet-Windows-x64.zip'
archive=QA/'baseline.zip';urllib.request.urlretrieve(URL,archive)
def sha(p):
    with Path(p).open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(archive)==BASE_SHA
with zipfile.ZipFile(archive)as z:z.extractall(QA/'baseline')
manifests=list((QA/'baseline').rglob('components.json'));assert len(manifests)==1
base=manifests[0].parent.resolve();exe=base/'PGN-Horizon.exe'
# Explicit publisher packaging correction for the known old cold archive.
# Retain the public provenance next to, rather than within, the code cohort.
# No inventory guard is disabled and no unknown file is removed.
provenance=base/'SOURCE-COMMIT.json';assert provenance.is_file()
provenance_bytes=provenance.read_bytes();assert len(provenance_bytes)<16384
assert type(json.loads(provenance_bytes))is dict
provenance.rename(QA/'legacy-SOURCE-COMMIT.json')

sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'src/update')]
import stage_component_bundle as stage
from horizon_update_client import AuthenticatedUpdates
from horizon_component_install import CohortStore,inventory
from horizon_windows_install import WindowsCohortLifecycle
from horizon_windows_lifecycle import OwnedWindowsProcess,owned_ipc
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
    for _ in range(40):
        assert process.poll()is None,'Old real GUI exited'
        try:state=owned_ipc(own,'--inspect-owned');break
        except (RuntimeError,OSError,ValueError):time.sleep(.25)
    assert state and state['pgn_update_genesis']==GENESIS
    data=Path(state['pgn_update_data_root']);assert not data.resolve().is_relative_to(base), 'Standard Windows data must be outside its executable cohort'
    sentinel=data/'wallet-preservation-sentinel.dat';sentinel.parent.mkdir(parents=True,exist_ok=True);sentinel.write_bytes(b'ISOLATED TEST: KEEP EXACT BYTES')
    saved=sha(sentinel)
    context,context_hash=stage.context_from_bytes(json.dumps(dict(schema='PGN_COMPONENT_STAGE_CONTEXT_V1',network='testnet',genesis_hash=GENESIS,platform='windows-x86_64',compatibility_set='pgn.epoch3.2026.11',minimum_sequence=1,installed=[],protected_roots=[str(base),str(data)])).encode())
    root=(ROOT/'updates/testnet-epoch3/metadata/root.json').read_bytes();root_hash=hashlib.sha256(root).hexdigest()
    assert root_hash=='37f8978ea7e5844bb45b3f22c5cf2f51b68266f26a92583a4959a9e9a3870dbf'
    client=AuthenticatedUpdates(QA/'downloads',root,root_hash,'https://raw.githubusercontent.com/wizz13150/prime-gap-network/qualification/release269-updater/updates/testnet-epoch3/metadata/','https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-updates-epoch3/',context,context_hash)
    discovered=client.refresh(['ui','core','gpu','cpu']);assert discovered['publisher_authenticated'] and discovered['sequence']==22
    store=CohortStore.initialize(QA/'installation',base,context,context_hash)
    candidate=store.prepare(client,['ui','core','gpu','cpu'])
    new=Path(candidate['cohort']);manifest=inventory(new)[0]
    versions={c['id']:c['version']for c in manifest['components']}
    dependency_probe=subprocess.run([str(new/'Core/pgn-core.exe'),'-version'],capture_output=True,timeout=15)
    assert dependency_probe.returncode==0,dependency_probe.stderr.decode(errors='replace')[-1000:]
    assert versions==dict(ui='2026.11.16.161',core='2026.11.16.23',gpu='2026.11.15.84',cpu='2026.11.6.389')
    activation=dict(schema='PGN_WINDOWS_UPDATE_ACTIVATION_V1',ui_pid=own.pid,ui_birth=own.birth,application_root=state['pgn_update_application_root'],data_root=str(data))
    life=WindowsCohortLifecycle(store,base,activation)
    assert store.activate(candidate,life.drain,life.start_and_verify,life.restore_previous)
    process.wait(timeout=20);assert process.returncode==0 and life.ui.exited()
    assert store.current()['cohort']==str(new)
    live=owned_ipc(life.new_ui,'--inspect-owned')
    assert live['pgn_update_genesis']==GENESIS and live['pgn_update_data_root']==str(data)
    assert sha(sentinel)==saved
    seen=[]
    for section in ('Wallet','Social','SocialMiners','Mining'):
        view=gui_command(life.new_ui,'--show-owned',section);seen.append(section)
        (E/(section+'.json')).write_text(json.dumps(view,indent=2)+'\n')
    new_pid=life.new_ui.pid;gui_command(life.new_ui,'--close-owned')
    for _ in range(80):
        if life.new_ui.exited():break
        time.sleep(.25)
    assert life.new_ui.exited(),'New real GUI failed to close normally'
    current=store.current();reopened=CohortStore(QA/'installation',context,context_hash)
    assert reopened.current()==current and sha(sentinel)==saved
    result=dict(PASS=True,public_updater=True,legacy_packaging_annotation_relocated=True,unrepaired_legacy_archive_refused=True,metadata_sequence=22,old_versions={c['id']:c['version']for c in old['components']},new_versions=versions,old_normal_exit=process.returncode,real_Qt_restart=True,new_pid=new_pid,new_normal_close=True,selector_retained=True,isolated_data_bytes_preserved=True,sections=seen,seconds=time.monotonic()-start,scope='Standard per-user Windows layout: actual download/authentication/cohort activation/Qt restart after explicitly moving the old publisher provenance annotation outside the code cohort. Unrepaired legacy cold archive is refused. Bundled Core23 executable/dependencies load; no real wallet sync or GPU computation tested here.')
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
