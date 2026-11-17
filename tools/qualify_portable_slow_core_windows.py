"""Exercise a delayed RPC in the real Windows app with an isolated existing wallet."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.request import Request, urlopen
import base64
import hashlib
import json
import os
import shutil
from struct import unpack
import subprocess
import sys
import tempfile
import time
import zipfile

GENESIS = "710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314"
ARCHIVE_HASH = "e9ae1479a2969356ff4a26af4c961046075d38d16fead9f27d446ed44d9f27d7"
PUBLIC_HISTORY_SHA = "09f318bfa826fad3d2af199ead9bb321dad039308864e549e3b92e6db6cfb7e4"
RELEASE = "https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Windows.zip?qualification=default-wallet-core29"
WALLET = "QA_Existing_Wallet"

ROOT = Path.cwd()
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "src/update"), str(ROOT / "tests")]
from horizon_component_install import inventory
from horizon_windows_lifecycle import OwnedWindowsProcess, ProcessAbsentError, owned_ipc
from native_grouped249_actual import Node

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream,"sha256").hexdigest()

def rpc(data, port, method, params, timeout=3):
    auth = base64.b64encode((data / "testnet/.cookie").read_bytes().strip()).decode("ascii")
    request = Request(f"http://127.0.0.1:{port}/",headers={"Authorization": "Basic "+auth,"Content-Type":"application/json"},
                      data=json.dumps({"jsonrpc":"2.0","id":1,"method":method,"params":params}).encode())
    with urlopen(request,timeout=timeout) as response:
        value=json.loads(response.read(2097152))
    if value.get("error") is not None:
        raise RuntimeError(value["error"])
    return value["result"]

def next_public_block(path):
    assert sha(path) == PUBLIC_HISTORY_SHA
    with path.open("rb") as stream:
        assert stream.read(23) == b"PGN-PUBLIC-HISTORY-249\x00"
        for height in (0,1):
            index,size = unpack("<II",stream.read(8))
            block_hash,raw = stream.read(32).hex(),stream.read(size)
            assert index == height and 0<size<=4000000 and len(raw)==size
            if height==0:
                assert block_hash==GENESIS
    return block_hash,raw.hex()

def stop_fixture_core(core, pid, data, port):
    try:
        owner=OwnedWindowsProcess(pid,core,sha(core))
    except ProcessAbsentError:
        return
    try:
        if owner.exited():
            return
        rpc(data,port,"stop",[])
        end=time.monotonic()+25
        while not owner.exited() and time.monotonic()<end:
            time.sleep(.25)
        assert owner.exited(),"Synthetic Core did not stop"
    finally:
        owner.close()

def use_portable_wallet(base, scratch, evidence, expect_timeout, address=None, next_block=None,
                        retain_core=False, previous_pid=None, check_delay=True, expected_height=None):
    roaming = scratch / "Roaming"
    local = scratch / "Local"
    roaming.mkdir(exist_ok=True)
    local.mkdir(exist_ok=True)
    data = roaming / "PGNCore" / "networks" / GENESIS[:16]
    core = base / "Core/pgn-core.exe"
    assert sha(core) == "6e0d827229ab23fc36f0957f53e43548f6f05437d826b15e5ed98ea0825335ce"
    wallet = data / "testnet/wallets" / WALLET / "wallet.dat"
    if address is None:
        assert not data.exists(), "The baseline must initialize only the isolated fixture"
        node = Node(core,data)
        try:
            assert node.rpc("getblockhash",[0])==GENESIS
            assert node.rpc("createwallet",[WALLET,False,False,"",False,True])["name"]==WALLET
            address = node.rpc("getnewaddress",["Existing wallet marker", "bech32"])
            assert isinstance(address,str) and len(address)>20
            assert wallet.is_file()
        finally:
            node.close()
        # Keep one known chain and one RPC executor in the synthetic fixture.
        conf = data / "pgn.conf"
        assert not conf.exists() or not conf.read_text(encoding="utf-8").strip()
        conf.write_text("rpcthreads=1\nnetworkactive=0\n", encoding="utf-8")
    else:
        assert wallet.is_file(), "Follow-up app starts with the actual preceding installation state"
    env = os.environ | {"APPDATA": str(roaming), "LOCALAPPDATA": str(local)}
    for name in ("QT_PLUGIN_PATH","QT_QPA_PLATFORM","QT_QPA_PLATFORM_PLUGIN_PATH"):
        env.pop(name,None)
    exe = base / "PGN-Horizon.exe"
    port = json.loads((base / "Core/core-template.json").read_bytes())["deployment"]["rpc_port"]
    process = None
    owned = None
    visible = {}
    managed_pid = None
    listed = None
    rpc_error = None
    receipt = None
    with (evidence / "stdout.log").open("wb") as out, (evidence / "stderr.log").open("wb") as err:
        try:
            process = subprocess.Popen([str(exe),"--testnet","--start-minimized"],cwd=base,env=env,
                                       stdin=subprocess.DEVNULL,stdout=out,stderr=err,creationflags=0x08000000)
            owned=OwnedWindowsProcess(process.pid,exe,sha(exe))
            end=time.monotonic()+110
            while time.monotonic()<end and process.poll() is None:
                try:
                    state=owned_ipc(owned,"--inspect-owned")
                    visible={key:state.get(key) for key in ("pgn_wallet_connected","pgn_wallet_embedded","pgn_core_link_state",
                                                            "pgn_core_link_detail","pgn_wallet_frontend_error","pgn_wallet_embedded_pid")}
                    managed_pid=visible["pgn_wallet_embedded_pid"]
                    listed=rpc(data,port,"listwallets",[])
                    rpc_error=None
                    # Wallet-side verified link plus the exact wallet on the
                    # expected Core cookie/port are authoritative. The mining
                    # canvas may be unmounted during a portable wallet launch.
                    if visible["pgn_core_link_state"]=="ready" and visible["pgn_wallet_embedded"] and listed==[WALLET]:
                        break
                except (RuntimeError,OSError,ValueError) as error:
                    rpc_error=type(error).__name__
                time.sleep(.5)
            else:
                raise RuntimeError(f"Expected wallet did not attach: state={visible}, wallets={listed}, rpc_error={rpc_error}")
            assert rpc(data,port,"getblockhash",[0])==GENESIS
            assert visible["pgn_wallet_embedded_pid"]>0
            if previous_pid is not None:
                assert visible["pgn_wallet_embedded_pid"] != previous_pid
            verified_core = OwnedWindowsProcess(managed_pid,core,sha(core))
            try:
                assert not verified_core.exited()
            finally:
                verified_core.close()
            assert [p.name for p in (data / "testnet/wallets").iterdir() if (p / "wallet.dat").is_file()]==[WALLET]
            first_state = owned_ipc(owned, "--inspect-owned")
            initial_transitions = first_state["pgn_core_link_transitions"]
            assert initial_transitions[-1]["state"] == "ready"
            tip = rpc(data, port, "getblockchaininfo", [])["bestblockhash"]
            samples = ["ready"]
            inspected = first_state
            hold_seconds = 0
            observed_timeout = False
            if check_delay:
                # A bounded, ordinary request occupies the isolated server for
                # longer than the released UI's three-second read timeout.
                with ThreadPoolExecutor(max_workers=1) as pool:
                    start = time.monotonic()
                    hold = pool.submit(rpc, data, port, "waitfornewblock", [7000, tip], 16)
                    samples = []
                    end_of_case = start + 13
                    while time.monotonic() < end_of_case:
                        inspected = owned_ipc(owned, "--inspect-owned")
                        samples.append(inspected["pgn_core_link_state"])
                        if hold.done() and samples[-1] == "ready":
                            break
                        time.sleep(.3)
                    assert hold.result(timeout=2)["hash"] == tip
                    hold_seconds = time.monotonic() - start
                assert 6.5 <= hold_seconds < 13.5, hold_seconds
                assert samples[-1] == "ready", samples
                observed_timeout = "rpc_unavailable" in samples
                assert observed_timeout == expect_timeout, (samples, inspected["pgn_core_link_transitions"])
            assert inspected["pgn_wallet_embedded_pid"] == managed_pid
            assert rpc(data,port,"listwallets",[]) == [WALLET]
            assert rpc(data,port,"getaddressinfo",[address])["ismine"] is True
            assert rpc(data,port,"getblockhash",[0]) == GENESIS
            advanced = None
            if expected_height is not None:
                assert rpc(data,port,"getblockcount",[]) == expected_height
            if next_block is not None:
                identity,payload = next_block
                assert rpc(data,port,"getblockcount",[]) == 0
                assert rpc(data,port,"submitblock",[payload],15) is None
                assert rpc(data,port,"getbestblockhash",[]) == identity
                end_of_chain = time.monotonic()+20
                while time.monotonic()<end_of_chain:
                    observed = owned_ipc(owned,"--inspect-owned")
                    if str(observed.get("pgn_wallet_height"))=="1":
                        assert observed["pgn_core_link_state"]=="ready"
                        assert observed["pgn_wallet_genesis"]==GENESIS
                        advanced=1
                        break
                    time.sleep(.4)
                assert advanced==1,"Horizon did not display the accepted block"
            receipt={"PASS": True,"ui_sha256":sha(exe),"displayed_height_after_recovery":advanced,
                     "while_delayed":samples,"expected_read_unavailable":expect_timeout,
                     "observed_read_unavailable":observed_timeout,"case_seconds":hold_seconds,
                     "same_core_process_and_wallet_after":True,"existing_address_still_owned":True,"core_from_portable_release": True,"ordinary_existing_wallet_discovered": True,
                     "wallet_and_core_connected": True,"same_default_core_directory": True,"identity": WALLET,
                     "gui_pid": process.pid,"managed_core_pid":visible["pgn_wallet_embedded_pid"],
                     "screen_control": False,"scope": "Actual portable Windows UI + Core29 in a fresh isolated profile, existing default wallet and controlled slow local RPC."}
        finally:
            if owned and not owned.exited():
                subprocess.run([str(exe),"--close-owned",str(owned.pid)],cwd=base,env=env,capture_output=True,timeout=15)
                try:
                    process.wait(timeout=40)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=15)
            if owned:
                owned.close()
            if managed_pid and (not retain_core or receipt is None):
                stop_fixture_core(core,managed_pid,data,port)
    assert process.returncode==0
    if retain_core:
        verified_core = OwnedWindowsProcess(managed_pid,core,sha(core))
        try:
            assert not verified_core.exited(), "Baseline owned Core was expected to survive its GUI"
        finally:
            verified_core.close()
    return receipt, address

def main():
    assert os.name=="nt" and os.environ.get("GITHUB_ACTIONS")=="true"
    assert len(sys.argv) == 4
    candidate = Path(sys.argv[1]).resolve(strict=True)
    expected_hash = sys.argv[2]
    next_block = next_public_block(Path(sys.argv[3]))
    assert sha(candidate) == expected_hash
    evidence=ROOT / "default-wallet-evidence"
    evidence.mkdir()
    with tempfile.TemporaryDirectory(prefix="pgn-qaw-",ignore_cleanup_errors=True) as tmp:
        scratch=Path(tmp)
        archive=scratch / "release.zip"
        with urlopen(RELEASE,timeout=120) as source, archive.open("xb") as dest:
            remaining=300*1024*1024
            while packet:=source.read(1024*1024):
                remaining-=len(packet)
                assert remaining>=0
                dest.write(packet)
        assert sha(archive)==ARCHIVE_HASH
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(scratch/"code")
        base=scratch/"code/PGN"
        assert (base/"components.json").is_file()
        released_ui = base / "PGN-Horizon.exe"
        assert sha(released_ui) != expected_hash
        profile = scratch/"existing"
        profile.mkdir()
        replacement = scratch/"new"/"PGN"
        shutil.copytree(base,replacement)
        replacement_ui = replacement/"PGN-Horizon.exe"
        replacement_ui.write_bytes(candidate.read_bytes())
        assert sha(replacement_ui) == expected_hash
        manifest_path = replacement/"components.json"
        manifest = json.loads(manifest_path.read_bytes())
        ui = next(component for component in manifest["components"] if component["id"]=="ui")
        entry = next(file for file in ui["files"] if file["path"]=="PGN-Horizon.exe")
        entry.update(bytes=replacement_ui.stat().st_size,sha256=expected_hash)
        manifest_path.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
        _, files, _, _ = inventory(replacement)
        assert files["pgn-horizon.exe"]["sha256"] == expected_hash
        released_ui_hash = sha(released_ui)
        baseline, address = use_portable_wallet(base, profile, evidence, expect_timeout=True,retain_core=True)
        original_pid = baseline["managed_core_pid"]
        try:
            assert sha(released_ui) == released_ui_hash
            trial, confirmed_address = use_portable_wallet(replacement, profile, evidence,
                                                         expect_timeout=False,address=address,next_block=next_block,
                                                         previous_pid=original_pid)
            assert address == confirmed_address and sha(released_ui) == released_ui_hash
            moved = scratch/"moved"/"PGN"
            moved.parent.mkdir()
            replacement.rename(moved)
            assert not replacement.exists()
            reopened, reopened_address = use_portable_wallet(moved, profile, evidence, expect_timeout=False,
                                                             address=address,check_delay=False,expected_height=1)
            assert reopened_address == address
        finally:
            original_data=profile/"Roaming"/"PGNCore"/"networks"/GENESIS[:16]
            port=json.loads((base/"Core/core-template.json").read_bytes())["deployment"]["rpc_port"]
            stop_fixture_core(base/"Core/pgn-core.exe",original_pid,original_data,port)
        receipt = {"PASS": baseline["PASS"] and trial["PASS"], "baseline": baseline, "candidate": trial, "reopened_after_moving_installation": reopened,
                   "archive_sha256": ARCHIVE_HASH, "same_user_profile": True, "old_installation_present": True,
                   "different_launch_path": True, "normal_launch": True, "adopted_new_core_and_stopped_old_owned_core": True,
                   "missing_previous_executable_recovered": True}
    (evidence/"RESULT.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(receipt))

if __name__=="__main__":
    main()
