"""Actual public TUF download, cohort install and real Qt restart on isolated Windows."""
from pathlib import Path
import hashlib,http.client,json,os,subprocess,sys,zipfile,time,urllib.request,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from shutil import copytree
from contextlib import closing,contextmanager
from pathlib import PurePosixPath
from shutil import disk_usage
from urllib3.exceptions import ProtocolError
ROOT=Path.cwd();QA=ROOT/'handoff-preflight';QA.mkdir();E=QA/'evidence';E.mkdir()
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

def verify_slow_core_handoff(current_code, expected):
    """Run a real portable GUI against the same recorded older owned process."""
    profile_path,=list(local.rglob('wallet-core.json'))+list(roaming.rglob('wallet-core.json'))
    original=profile_path.read_bytes()
    profile=json.loads(original)
    assert Path(profile['data_directory']).resolve()==wallet_data.resolve()
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


outputs={}
with small_update_volume(QA) as volume:
    outputs['low_space_volume_mib']=disk_usage(volume).free//1048576
    assert outputs['low_space_volume_mib']<225
base_gui=None;base_owner=None;start_core_pid=None
try:
    base_gui=subprocess.Popen([str(exe),'--testnet','--start-minimized'],cwd=base,creationflags=0x08000000)
    base_owner=OwnedWindowsProcess(base_gui.pid,exe,sha(exe))
    cutoff=time.monotonic()+85
    while time.monotonic()<cutoff and not base_owner.exited():
        try:
            view=owned_ipc(base_owner,'--inspect-owned')
            if view.get('pgn_core_link_state')=='ready' and view.get('pgn_wallet_embedded_pid'):
                start_core_pid=view['pgn_wallet_embedded_pid']
                break
        except (RuntimeError,ValueError,OSError):
            pass
        time.sleep(.25)
    assert start_core_pid
    assert rpc(wallet_data,wallet_port,'getaddressinfo',[address])['ismine']
finally:
    if base_owner and not base_owner.exited():
        gui_command(base_owner,'--close-owned')
        base_gui.wait(timeout=35)
    if base_owner:
        base_owner.close()
    if start_core_pid:
        stop_fixture_core(base/'Core/pgn-core.exe',start_core_pid,wallet_data,wallet_port)
standin=QA/'candidate'
copytree(base,standin)
candidate=Path(sys.argv[1]).resolve(strict=True)
assert candidate.is_file()
built_source=(candidate.parent/'SOURCE.txt').read_text().strip()
assert built_source=='91a9bc1d5419b51f44b6b05a4a352b6459bc00cc'
assert (candidate.parent/'SHA256SUMS.txt').read_text().split()[0]==sha(candidate)
(standin/'PGN-Horizon.exe').write_bytes(candidate.read_bytes())
manifest_path=standin/'components.json'
manifest=json.loads(manifest_path.read_bytes())
ui=next(c for c in manifest['components'] if c['id']=='ui')
ui['version']='2026.11.17.177'
binary=next(e for e in ui['files'] if e['path']=='PGN-Horizon.exe')
binary.update(bytes=candidate.stat().st_size,sha256=sha(candidate))
manifest_path.write_text(json.dumps(manifest),encoding='utf-8')
outputs['old']=verify_slow_core_handoff(base,False)
outputs['candidate']=verify_slow_core_handoff(standin,True)
outputs['PASS']=True
(E/'PLATFORM-PREFLIGHT.json').write_text(json.dumps(outputs,indent=2)+'\n',encoding='utf-8')
print(json.dumps(outputs))
