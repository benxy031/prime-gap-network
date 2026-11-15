"""Preserve exact public-base native link inputs; never imports private Gold."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
parser=argparse.ArgumentParser();parser.add_argument('--ninja',required=True);parser.add_argument('--git',default='git');args=parser.parse_args()
build=Path('build-native');out=Path('native-sdk');out.mkdir();files={}
for p in sorted(build.rglob('*')):
 if not p.is_file()or p.is_symlink():continue
 rel=p.relative_to(build)
 if p.suffix not in('.a','.lib','.h','.hpp')or'CMakeFiles'in rel.parts:continue
 target=out/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
 files[rel.as_posix()]=dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
target='pgn-native-audit-replay-test';unit='native_audit_replay_test.cpp'
commands=subprocess.run([args.ninja,'-C',str(build),'-t','commands',target],capture_output=True,check=True,timeout=20).stdout.decode('utf-8')
recipes=[line for line in commands.splitlines()if unit in line];assert len(recipes)==2 and files
value=dict(schema='PGN_PRIVATE_NATIVE_TEST_SDK_V1',source=subprocess.check_output([args.git,'rev-parse','HEAD'],text=True).strip(),target=target,unit=unit,files=files,recipes=recipes,build_root=str(build.resolve()),source_root=str(Path('wallet/riecoin-core-upstream-v2606').resolve()),purpose='Private link inputs from public base plus R205 only; private Gold TU absent; not complete product qualification')
(out/'NATIVE-SDK.json').write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(files=len(files),bytes=sum(v['bytes']for v in files.values()),recipes=len(recipes),target=target)))
