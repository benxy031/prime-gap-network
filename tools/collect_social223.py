"""Bounded private qualification artifacts: executable/source receipts, NO wallets."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--build-outcome',required=True);p.add_argument('--test-outcome',required=True);a=p.parse_args()
out=Path('private-social223-delivery');out.mkdir(exist_ok=True)
git=shutil.which('git') or r'C:/Program Files/Git/bin/git.exe'
pin='1ea788c04d55f0bb06810ac8ff8c5961e0f0872d'
head=subprocess.check_output([git,'rev-parse','HEAD'],text=True).strip()
changes=subprocess.check_output([git,'diff','--name-only',pin,head],text=True).splitlines()
files={}
def copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target);files[target.relative_to(out).as_posix()]=dict(bytes=target.stat().st_size,sha256=hashlib.sha256(target.read_bytes()).hexdigest())
for source in Path('private-social223/Core').glob('*'):
    if source.suffix.lower() in ['.exe','.dll']:copy(source,out/'Core'/source.name)
for name in ['build-resource.json','test-resource.json','build-private.log','private-social223/dependencies.txt','private-social223/private-native-test/RESULT.json']:
    source=Path(name)
    if source.is_file():copy(source,out/'receipts'/source.name)
for name in changes:
    source=Path(name)
    if source.is_file():copy(source,out/'source-delta'/name)
(out/'MANIFEST.json').write_text(json.dumps(dict(schema='PGN_PRIVATE_SOCIAL223_QUALIFICATION_1',source=head,base=pin,build_outcome=a.build_outcome,test_outcome=a.test_outcome,public_activation=False,production_authority=False,mock_crypto=False,files=files,limits=['Native private2-peer witness only; Qt action witness separate','No public ordering/recovery/governance authority selected','No wallet/privatekey/cookie/datadir copied']),indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(source=head,build=a.build_outcome,test=a.test_outcome,files=len(files))))
