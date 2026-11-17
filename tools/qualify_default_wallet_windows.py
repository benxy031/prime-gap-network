"""Verify the portable Windows build discovers and starts an existing default wallet."""
from pathlib import Path
from urllib.request import Request, urlopen
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import zipfile

GENESIS = "710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314"
ARCHIVE_HASH = "e9ae1479a2969356ff4a26af4c961046075d38d16fead9f27d446ed44d9f27d7"
RELEASE = "https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Windows.zip?qualification=default-wallet-core29"
WALLET = "QA_Existing_Wallet"

ROOT = Path.cwd()
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "src/update"), str(ROOT / "tests")]
from horizon_windows_lifecycle import OwnedWindowsProcess, owned_ipc
from native_grouped249_actual import Node

def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream,"sha256").hexdigest()

def rpc(data, port, method, params):
    auth = base64.b64encode((data / "testnet/.cookie").read_bytes().strip()).decode("ascii")
    request = Request(f"http://127.0.0.1:{port}/",headers={"Authorization": "Basic "+auth,"Content-Type":"application/json"},
                      data=json.dumps({"jsonrpc":"2.0","id":1,"method":method,"params":params}).encode())
    with urlopen(request,timeout=3) as response:
        value=json.loads(response.read(2097152))
    if value.get("error") is not None:
        raise RuntimeError(value["error"])
    return value["result"]

def use_portable_wallet(base, scratch, evidence):
    roaming = scratch / "Roaming"
    local = scratch / "Local"
    roaming.mkdir()
    local.mkdir()
    data = roaming / "PGNCore" / "networks" / GENESIS[:16]
    core = base / "Core/pgn-core.exe"
    assert sha(core) == "6e0d827229ab23fc36f0957f53e43548f6f05437d826b15e5ed98ea0825335ce"
    node = Node(core,data)
    try:
        assert node.rpc("getblockhash",[0])==GENESIS
        assert node.rpc("createwallet",[WALLET,True,True,"",False,True])["name"]==WALLET
        wallet = data / "testnet/wallets" / WALLET / "wallet.dat"
        assert wallet.is_file()
    finally:
        node.close()
    # The application sees only a fresh Windows profile. The fixture already
    # contains one wallet and a valid initialized Core data directory.
    env = os.environ | {"APPDATA": str(roaming), "LOCALAPPDATA": str(local)}
    for name in ("QT_PLUGIN_PATH","QT_QPA_PLATFORM","QT_QPA_PLATFORM_PLUGIN_PATH"):
        env.pop(name,None)
    exe = base / "PGN-Horizon.exe"
    port = json.loads((base / "Core/core-template.json").read_bytes())["deployment"]["rpc_port"]
    process = None
    owned = None
    visible = {}
    with (evidence / "stdout.log").open("wb") as out, (evidence / "stderr.log").open("wb") as err:
        try:
            process = subprocess.Popen([str(exe),"--testnet","--start-minimized","--update-healthcheck"],cwd=base,env=env,
                                       stdin=subprocess.DEVNULL,stdout=out,stderr=err,creationflags=0x08000000)
            owned=OwnedWindowsProcess(process.pid,exe,sha(exe))
            end=time.monotonic()+110
            while time.monotonic()<end and process.poll() is None:
                try:
                    state=owned_ipc(owned,"--inspect-owned")
                    visible={key:state.get(key) for key in ("pgn_wallet_connected","pgn_wallet_embedded","pgn_core_link_state",
                                                            "pgn_core_link_detail","pgn_wallet_frontend_error","pgn_wallet_embedded_pid")}
                    loaded=rpc(data,port,"listwallets",[])
                    if visible["pgn_wallet_connected"] and visible["pgn_wallet_embedded"] and loaded==[WALLET]:
                        break
                except (RuntimeError,OSError,ValueError):
                    pass
                time.sleep(.5)
            else:
                raise RuntimeError(f"Existing default wallet failed to connect automatically: {visible}")
            assert rpc(data,port,"getblockhash",[0])==GENESIS
            assert visible["pgn_wallet_embedded_pid"]>0
            assert [p.name for p in (data / "testnet/wallets").iterdir() if (p / "wallet.dat").is_file()]==[WALLET]
            receipt={"PASS": True,"core_from_portable_release": True,"ordinary_existing_wallet_discovered": True,
                     "wallet_and_core_connected": True,"same_default_core_directory": True,"identity": WALLET,
                     "gui_pid": process.pid,"managed_core_pid":visible["pgn_wallet_embedded_pid"],
                     "screen_control": False,"scope": "Fresh isolated Windows profile, existing default wallet, normal application start."}
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
    assert process.returncode==0
    return receipt

def main():
    assert os.name=="nt" and os.environ.get("GITHUB_ACTIONS")=="true"
    evidence=ROOT / "default-wallet-evidence"
    evidence.mkdir()
    with tempfile.TemporaryDirectory(prefix="pgn-qaw-") as tmp:
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
        receipt=use_portable_wallet(base,scratch,evidence)
    (evidence/"RESULT.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(receipt))

if __name__=="__main__":
    main()
