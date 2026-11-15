"""Reuse the exact already-built Core; bearer authentication stays on API only."""
from pathlib import Path, PurePosixPath
import hashlib, json, os, time, urllib.request, urllib.error, zipfile
RUN=43675733482
SOURCE='9369dee94981f3a9011b45c1ebb68b897e44d6d3'
CPP_SHA='00fc328b6bb38580cf9918c97dd1163eb2b0f8de883704627b8d67a7332a0c3d'
API='https://api.github.com/repos/wizz13150/prime-gap-network'
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl):return None
opener=urllib.request.build_opener(NoRedirect())
def request(path):
    return urllib.request.Request(API+path,headers={'Authorization':'Bearer '+os.environ['GITHUB_TOKEN'],'Accept':'application/vnd.github+json','X-GitHub-Api-Version':'2022-11-28'})
def read(path):
    with opener.open(request(path),timeout=30)as response:return json.load(response)
def main():
    cpp=Path('wallet/riecoin-core-upstream-v2606/src/rpc/pgn_native.cpp')
    assert hashlib.sha256(cpp.read_bytes()).hexdigest()==CPP_SHA,'Core RPC source changed; rebuild required'
    run=read('/actions/runs/'+str(RUN));assert run['head_sha']==SOURCE
    deadline=time.monotonic()+600
    while True:
        values=read('/actions/runs/'+str(RUN)+'/artifacts')['artifacts']
        artifacts=[a for a in values if a['name']=='native-audit-batch232' and not a['expired']]
        if artifacts:break
        run=read('/actions/runs/'+str(RUN))
        if run['status']=='completed' or time.monotonic()>=deadline:
            raise RuntimeError('Required immutable Core build artifact unavailable')
        time.sleep(10)
    assert len(artifacts)==1
    artifact=artifacts[0];expected=artifact['digest'];assert expected.startswith('sha256:')
    try:
        opener.open(request('/actions/artifacts/'+str(artifact['id'])+'/zip'),timeout=30)
        raise RuntimeError('Expected API archive redirect')
    except urllib.error.HTTPError as response:
        assert response.code==302
        location=response.headers['Location'];assert location.startswith('https://')
    # Deliberately a fresh request/opener: no bearer header reaches storage.
    target=Path('batch232');target.mkdir(exist_ok=True)
    archive=target/'core.zip'
    with urllib.request.urlopen(urllib.request.Request(location),timeout=60)as response,archive.open('wb')as stream:
        while chunk:=response.read(1024*1024):stream.write(chunk)
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==expected.split(':',1)[1]
    matches=[]
    with zipfile.ZipFile(archive)as zipped:
        for entry in zipped.infolist():
            parts=PurePosixPath(entry.filename).parts
            assert not entry.filename.startswith(('/', '\\')) and '..' not in parts and ':' not in entry.filename and '\\' not in entry.filename
            if parts[-1:] == ('riecoind',):matches.append(entry)
        assert len(matches)==1
        core=target/'riecoind';core.write_bytes(zipped.read(matches[0]));core.chmod(0o755)
    archive.unlink()
    receipt=dict(source=SOURCE,run=RUN,artifact_id=artifact['id'],archive_digest=expected,core_rpc_sha256=CPP_SHA,executable_sha256=hashlib.sha256(core.read_bytes()).hexdigest(),build_repeated=False)
    (target/'CORE-REUSE.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(receipt))
if __name__=='__main__':
    try:main()
    except urllib.error.URLError as error:raise SystemExit('Artifact transport failed: '+type(error).__name__)
