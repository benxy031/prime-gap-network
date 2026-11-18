from pathlib import Path,PurePosixPath
import hashlib,json,sys,zipfile
from check_public_release import inspect

bundle,manifest,path=map(Path,sys.argv[1:4])
def sha(file):
    with file.open('rb') as data:return hashlib.file_digest(data,'sha256').hexdigest()

assert sha(bundle)=='6afcb8cff7bce8b518116fca0abc6c3396ad9cbb594e965c97f4baa8126155a4' and sha(manifest)=='2e2baf8ac363ce3f42b81833a5e5553a5bd6bea498798f865faf41155435d2c7'
m=json.loads(manifest.read_bytes())
assert m['release_version']=='2026.11.18.182'
prefix=dict(ui='',core='Core/',gpu='EngineRuntime/',cpu='EngineRuntime/cpu/')
rows={}
for component in m['components']:
    ident=component['id']
    for row in component['files']:
        relative=prefix[ident]+row['path']
        pure=PurePosixPath(relative)
        assert not pure.is_absolute() and '..' not in pure.parts and chr(92) not in relative
        name='payload/'+ident+'/'+relative
        assert name not in rows
        rows[name]=(relative,row)
assert len(rows)==424

source='7af0b29ede822aa3d7b0ca8e91b37c89b9156e56'
provenance=dict(schema='PGN_SOURCE_PROVENANCE_V1',source_commit=source,source_tree='486ab0324ffaa4a31fb991d7ffb15be124a2c859',
    genesis_hash='710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314',
    compatibility_set='pgn.epoch3.2026.11',trust_root_sha256='37f8978ea7e5844bb45b3f22c5cf2f51b68266f26a92583a4959a9e9a3870dbf',
    component_versions={c['id']:c['version'] for c in m['components']},
    qualification=dict(signed_upgrade=44387773050,composed_gui=44386198347,frozen_engine=44383215852))
def record(name):
    item=zipfile.ZipInfo(name,(2026,11,18,0,0,0))
    item.compress_type=zipfile.ZIP_DEFLATED
    return item
path.parent.mkdir(exist_ok=True,parents=True)
with zipfile.ZipFile(bundle) as archive,zipfile.ZipFile(path,'x') as portable:
    names=archive.namelist()
    assert len(names)==425 and set(names)==set(rows)|{'component-bundle.json'}
    for name,(relative,row) in sorted(rows.items(),key=lambda value:value[1][0]):
        item=archive.getinfo(name)
        assert item.file_size==row['bytes']
        with archive.open(item) as src,portable.open(record('PGN/'+relative),'w') as dest:
            h=hashlib.sha256()
            while chunk:=src.read(1024*1024):
                h.update(chunk)
                dest.write(chunk)
            assert h.hexdigest()==row['sha256'],name
    portable.writestr(record('PGN/components.json'),manifest.read_bytes())
    portable.writestr(record('SOURCE-COMMIT.json'),json.dumps(provenance,indent=2))
assert inspect(path)['PASS']
report=dict(PASS=True,archive=path.name,sha256=sha(path),bytes=path.stat().st_size,files=426,source=source,manifest=sha(manifest))
(path.parent/'BUILD.json').write_text(json.dumps(report,indent=2),encoding='utf8')
