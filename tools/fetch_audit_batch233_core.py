"""Fetch the exact Core20 component; API auth never follows the redirect."""
from pathlib import Path,PurePosixPath
import argparse,hashlib,json,os,time,urllib.request,urllib.error,zipfile
COMPONENTS={'ubuntu':(43678338649,'9f0225dbe12ac12e30312334d772b2c561bddd7d'),'windows':(43680097035,'2677fb63a8ccc97e6b1ba7d6dc81a01919755afd')}
API='https://api.github.com/repos/wizz13150/prime-gap-network'
class NoRedirect(urllib.request.HTTPRedirectHandler):
 def redirect_request(self,req,fp,code,msg,headers,newurl):return None
opener=urllib.request.build_opener(NoRedirect())
def request(path):return urllib.request.Request(API+path,headers={'Authorization':'Bearer '+os.environ['GITHUB_TOKEN'],'Accept':'application/vnd.github+json','X-GitHub-Api-Version':'2022-11-28'})
def read(path):
 with opener.open(request(path),timeout=30)as response:return json.load(response)
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--platform',choices=('windows','ubuntu'),required=True);args=parser.parse_args()
 run_id,source_id=COMPONENTS[args.platform]
 run=read('/actions/runs/'+str(run_id));assert run['head_sha']==source_id
 deadline=time.monotonic()+900
 while True:
  artifacts=[v for v in read('/actions/runs/'+str(run_id)+'/artifacts')['artifacts']if v['name']=='audit-batch233-core-'+args.platform and not v['expired']]
  if artifacts:break
  run=read('/actions/runs/'+str(run_id))
  if run['status']=='completed'or time.monotonic()>=deadline:raise RuntimeError('Required immutable Core20 artifact unavailable')
  time.sleep(10)
 assert len(artifacts)==1;artifact=artifacts[0]
 try:
  opener.open(request('/actions/artifacts/'+str(artifact['id'])+'/zip'),timeout=30);raise RuntimeError('Archive redirect required')
 except urllib.error.HTTPError as response:
  if response.code!=302:raise RuntimeError('Core archive API HTTP '+str(response.code))from None
  location=response.headers['Location'];assert location.startswith('https://')
 out=Path('qa233');out.mkdir(exist_ok=True);archive=out/'core.zip'
 with urllib.request.urlopen(urllib.request.Request(location),timeout=60)as response,archive.open('wb')as stream:
  while chunk:=response.read(1024*1024):stream.write(chunk)
 assert'sha256:'+hashlib.sha256(archive.read_bytes()).hexdigest()==artifact['digest']
 with zipfile.ZipFile(archive)as zipped:
  for entry in zipped.infolist():
   parts=PurePosixPath(entry.filename).parts
   assert not entry.filename.startswith(('/', '\\'))and'..'not in parts and':'not in entry.filename and'\\'not in entry.filename
   if entry.is_dir():continue
   target=out/entry.filename;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(zipped.read(entry))
 source=json.loads((out/'CORE-SOURCE.json').read_bytes());assert source['source']==source_id and source['component_version']=='2026.11.15.20'
 for name,expected in source['inputs'].items():
  path=Path(name);raw=os.readlink(path).encode()if path.is_symlink()else path.read_bytes()
  assert hashlib.sha256(raw).hexdigest()==expected,'Core source differs from frozen component: '+name
 for name,row in source['files'].items():
  target=out/name;assert hashlib.sha256(target.read_bytes()).hexdigest()==row['sha256']
  if os.name!='nt':target.chmod(0o755)
 archive.unlink()
 receipt=dict(source=source_id,run=run_id,artifact_id=artifact['id'],artifact_digest=artifact['digest'],core_inputs=len(source['inputs']),source_inputs_exact=True,rebuild=False)
 (out/'CORE-REUSE.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print(json.dumps(receipt))
if __name__=='__main__':
 try:main()
 except urllib.error.URLError as error:raise SystemExit('Core artifact transport failed: '+type(error).__name__)
