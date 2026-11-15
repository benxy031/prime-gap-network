"""Run only a changed native fixture against byte-verified compiled libraries."""
from pathlib import Path
import hashlib,json,os,shlex,shutil,subprocess,sys,urllib.request,urllib.error,zipfile
SOURCE='1961641b6204c66bc81feefc4d4409fb2a80bc27';ARTIFACT=14145054384
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):return None
token=os.environ.pop('GH_TOKEN');headers={'Authorization':'Bearer '+token,'User-Agent':'PGN-native-fixture'}
url='https://api.github.com/repos/wizz13150/prime-gap-network/actions/artifacts/'+str(ARTIFACT)
with urllib.request.urlopen(urllib.request.Request(url,headers=headers),timeout=30)as f:info=json.load(f)
req=urllib.request.Request(url+'/zip',headers=headers)
try:response=urllib.request.build_opener(NoRedirect()).open(req,timeout=30)
except urllib.error.HTTPError as e:
    if e.code!=302:raise
    response=urllib.request.urlopen(e.headers['Location'],timeout=60)
with response as f,Path('sdk.zip').open('xb')as out:shutil.copyfileobj(f,out)
assert hashlib.sha256(Path('sdk.zip').read_bytes()).hexdigest()==info['digest'].split(':')[1]
with zipfile.ZipFile('sdk.zip')as z:
    assert all(not Path(x).is_absolute()and '..'not in Path(x).parts for x in z.namelist());z.extractall('sdk')
m=json.loads(Path('sdk/NATIVE-SDK.json').read_bytes());assert m['source']==SOURCE
for name,row in m['files'].items():
    p=Path('sdk')/name;assert p.stat().st_size==row['bytes']and hashlib.sha256(p.read_bytes()).hexdigest()==row['sha256']
    target=Path('build-native')/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
commands=m['recipes'];assert len(commands)==2
original=m['source_root'];cwd=str(Path.cwd());old_root=original[:-len('/wallet/riecoin-core-upstream-v2606')]
log=[]
for command in commands:
    command=command.replace(old_root,cwd)
    args=shlex.split(command)
    if args[:2]==[':','&&']:args=args[2:]
    if args[-2:]==['&&',':']:args=args[:-2]
    assert not any(x in(';','&&','||','|')for x in args)
    build=Path('build-native');(build/'bin').mkdir(exist_ok=True)
    for part in args:
        if part.endswith('.cpp.o'):
            p=Path(part);p=p if p.is_absolute()else build/p;p.parent.mkdir(parents=True,exist_ok=True)
    r=subprocess.run(args,cwd=build,capture_output=True,text=True,timeout=120);log.append(dict(exit=r.returncode,stdout=r.stdout[-4000:],stderr=r.stderr[-8000:]));assert r.returncode==0,log[-1]
exe=Path('build-native/bin/pgn-native-ticket-gold-group-test');rows=[]
for name,flag in(('passed',None),('missing','--one-auditor'),('failed','--failed-auditor')):
    args=[str(exe),str(Path('trial-'+name).resolve())]+([flag]if flag else[])
    r=subprocess.run(args,capture_output=True,text=True,timeout=60);rows.append(dict(name=name,exit=r.returncode,stdout=r.stdout[-4000:],stderr=r.stderr[-4000:]));assert r.returncode==0,rows[-1]
value=dict(PASS=True,source_sdk=SOURCE,artifact=ARTIFACT,libraries_verified=len(m['files']),native_rebuilt=False,compile=log,cases=rows,scope='Actual linked native fixture execution; not public consensus, full UTXO ConnectBlock or activation')
Path('SDK-TRIAL.json').write_text(json.dumps(value,indent=2)+'\n');print(json.dumps(value))
