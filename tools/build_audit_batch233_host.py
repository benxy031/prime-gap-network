"""Freeze host84 only; public CPU389 and scientific images are not rebuilt."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys
root=Path.cwd();out=root/'host233';out.mkdir(exist_ok=True)
versions=json.loads((root/'tools/component_versions.json').read_bytes())
assert versions==dict(ui='2026.11.15.157',core='2026.11.15.20',gpu='2026.11.15.84',cpu='2026.11.6.389')
subprocess.run([sys.executable,'-B','tools/build_horizon_engine_host.py','--output',str(out/'build')],check=True)
source=json.loads((out/'build/ENGINE-HOST-SOURCE.json').read_bytes());assert source['component_version']==versions['gpu']
for name,expected in source['sources'].items():
 assert hashlib.sha256((root/name).read_bytes()).hexdigest()==expected,'Frozen host source bytes changed: '+name
stage=out/'runtime/EngineRuntime/network';stage.parent.mkdir(parents=True,exist_ok=True)
shutil.copytree(out/'build/dist/horizon-engine-host',stage)
binary=stage/('horizon-engine-host.exe'if os.name=='nt'else'horizon-engine-host')
version=subprocess.check_output([str(binary),'--version'],text=True,timeout=20);assert versions['gpu']in version
raw=subprocess.check_output([str(binary),'--audit-batch-self-test'],text=True,timeout=60)
selftest=json.loads(raw);assert selftest['passed']and selftest['checks']==17 and selftest['no_network']and selftest['no_child_processes']and not selftest['cpu_activated']and not selftest['scientific_qualification']
(out/'HOST-SELFTEST.json').write_text(json.dumps(selftest,indent=2)+'\n',encoding='utf-8')
shutil.copyfile(out/'build/ENGINE-HOST-SOURCE.json',out/'ENGINE-HOST-SOURCE.json')
files={p.relative_to(out/'runtime').as_posix():dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())for p in sorted((out/'runtime').rglob('*'))if p.is_file()}
(out/'BINARY-FILES.json').write_text(json.dumps(files,indent=2)+'\n',encoding='utf-8')
receipt=dict(source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),versions=versions,host_executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),source_inputs_exact=True,closure_files=len(files),cpu389_unchanged=True,cpu_frozen=False,scientific_images_rebuilt=False,scope='Audit grouping control only; no scientific CPU qualification or activation')
(out/'BUILD-RECEIPT.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print(json.dumps(receipt))
