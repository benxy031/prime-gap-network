"""Preserve the exact compiled native libraries for small isolated follow-up tests."""
from pathlib import Path
import hashlib,json,shutil,subprocess
build=Path('build-native');out=Path('native-sdk');out.mkdir();files={}
for p in sorted(build.rglob('*')):
    if not p.is_file()or p.is_symlink():continue
    rel=p.relative_to(build)
    if p.suffix not in('.a','.lib','.h','.hpp')or any(x in rel.parts for x in('group-passed','group-missing','group-failed','CMakeFiles')):continue
    target=out/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
    files[rel.as_posix()]=dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
commands=subprocess.run(['ninja','-C',str(build),'-t','commands','pgn-native-ticket-gold-group-test'],capture_output=True,check=True,timeout=20).stdout.decode()
recipes=[line for line in commands.splitlines()if 'native_ticket_gold_group_test.cpp'in line]
assert len(recipes)==2 and files
value=dict(schema='PGN_PRIVATE_NATIVE_TEST_SDK_V1',source=subprocess.check_output(['git','rev-parse','HEAD']).decode().strip(),files=files,recipes=recipes,build_root=str(build.resolve()),source_root=str(Path('wallet/riecoin-core-upstream-v2606').resolve()),purpose='Private exact native link inputs; no wallets, chain data, runtime code promotion or keys')
(out/'NATIVE-SDK.json').write_text(json.dumps(value,indent=2)+'\n')
print(json.dumps(dict(files=len(files),bytes=sum(v['bytes']for v in files.values()),recipes=len(recipes))))
