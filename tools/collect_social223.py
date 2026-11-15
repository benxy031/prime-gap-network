"""Bounded private qualification artifacts: executable/source receipts, NO wallets."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--build-outcome',required=True);p.add_argument('--test-outcome',required=True);a=p.parse_args()
out=Path('private-social223-delivery');out.mkdir(exist_ok=True)
git=shutil.which('git') or r'C:/Program Files/Git/bin/git.exe'
pin='840f5168022084f81ff9a819e67cd646c339ea73'
head=subprocess.check_output([git,'rev-parse','HEAD'],text=True).strip()
# Preflight explicitly fetches the base before the expensive build. Independently
# preserve a compiled product even when a later source/test receipt fails.
diff=subprocess.run([git,'diff','--name-only',pin,head],capture_output=True,text=True)
changes=diff.stdout.splitlines() if diff.returncode==0 else []
files={}
def copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target);files[target.relative_to(out).as_posix()]=dict(bytes=target.stat().st_size,sha256=hashlib.sha256(target.read_bytes()).hexdigest())
for source in Path('private-social223-delivery/Core').glob('*'):
    if source.suffix.lower() in ['.exe','.dll']:files[source.relative_to(out).as_posix()]=dict(bytes=source.stat().st_size,sha256=hashlib.sha256(source.read_bytes()).hexdigest())
for name in ['consent-preflight-resource.json','consent-preflight.log','consent-preflight-targets.json','components-build-resource.json','components-build.log','build-resource.json','test-resource.json','public-off-resource.json','private-build-resource.json','private-mode-build.log','build-private.log','private-public-default-off/RESULT.json','private-social223/dependencies.txt','private-social223/private-native-test/RESULT.json']:
    name=name.replace('private-social223/private-native-test/','private-social223-test/')
    source=Path(name)
    if source.is_file():copy(source,out/'receipts'/source.name)
for name in changes:
    source=Path(name)
    if source.is_file():copy(source,out/'source-delta'/name)
for source in (out/'public-default-off').rglob('*'):
    if source.is_file():files[source.relative_to(out).as_posix()]=dict(bytes=source.stat().st_size,sha256=hashlib.sha256(source.read_bytes()).hexdigest())
for source in Path('private-social223-test').glob('*/process.log'):
    if source.is_file():
        raw=source.read_bytes();target=out/'receipts'/('Core-'+source.parent.name+'-process.log');target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw[-65536:]);files[target.relative_to(out).as_posix()]=dict(bytes=target.stat().st_size,sha256=hashlib.sha256(target.read_bytes()).hexdigest(),original_bytes=len(raw),bounded_tail=True)
for source in (out/'components').rglob('*'):
    if source.is_file():files[source.relative_to(out).as_posix()]=dict(bytes=source.stat().st_size,sha256=hashlib.sha256(source.read_bytes()).hexdigest())
(out/'MANIFEST.json').write_text(json.dumps(dict(schema='PGN_PRIVATE_SOCIAL223_QUALIFICATION_1',source=head,base=pin,source_diff_exit=diff.returncode,source_diff_error=diff.stderr,build_outcome=a.build_outcome,test_outcome=a.test_outcome,public_activation=False,production_authority=False,mock_crypto=False,files=files,limits=['Native private2-peer witness only; Qt action witness separate','No public ordering/recovery/governance authority selected','No wallet/privatekey/cookie/datadir copied']),indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(source=head,build=a.build_outcome,test=a.test_outcome,files=len(files))))
