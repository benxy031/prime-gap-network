"""Freeze only host/CPU control over exact previously-qualified native images."""
from pathlib import Path,PurePosixPath
import argparse,hashlib,json,os,shutil,subprocess,sys,tarfile,urllib.request,zipfile

def sha(path):
 with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def run(*args):subprocess.run([str(x)for x in args],check=True)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
 root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
 platform='windows-x86_64'if os.name=='nt'else'linux-x86_64';descriptor=root/'tools/cpu_service231_inputs.json';inputs=json.loads(descriptor.read_bytes())[platform]
 if not inputs['url'].startswith('https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/'):raise ValueError('Exact qualified public input URL required')
 archive=out/('qualified.zip'if os.name=='nt'else'qualified.tar.gz');request=urllib.request.Request(inputs['url'],headers={'User-Agent':'PGN-control-freeze231'})
 with urllib.request.urlopen(request,timeout=90)as response,archive.open('wb')as target:
  remaining=inputs['archive_bytes'];digest=hashlib.sha256()
  while remaining:
   data=response.read(min(1024*1024,remaining))
   if not data:raise ValueError('Truncated qualified input archive')
   target.write(data);digest.update(data);remaining-=len(data)
  if response.read(1):raise ValueError('Qualified input archive length differs')
 if digest.hexdigest()!=inputs['archive_sha256']:raise ValueError('Qualified input archive fingerprint differs')
 native=out/'native-inputs';native.mkdir();selected={}
 if os.name=='nt':
  source=zipfile.ZipFile(archive);entries=source.infolist();name=lambda e:e.filename;plain=lambda e:not e.is_dir()and(e.external_attr>>16)&0o170000!=0o120000;open_entry=source.open
 else:
  source=tarfile.open(archive,'r:gz');entries=source.getmembers();name=lambda e:e.name;plain=lambda e:e.isfile();open_entry=source.extractfile
 with source:
  for expected in inputs['native']:
   basename=PurePosixPath(expected['path']).name;suffix='/EngineRuntime/cpu/_internal/native/'+basename
   matches=[entry for entry in entries if('/'+name(entry)).endswith(suffix)]
   if len(matches)!=1:raise ValueError('Qualified native image absent or ambiguous: '+basename)
   item=matches[0];parts=PurePosixPath(name(item)).parts
   if '\\'in name(item)or PurePosixPath(name(item)).is_absolute()or any(p in('..','.')or':'in p for p in parts)or not plain(item):raise ValueError('Unsafe qualified native entry')
   target=native/basename
   with open_entry(item)as stream,target.open('wb')as output:
    raw=stream.read(expected['bytes']+1)
    if len(raw)!=expected['bytes']or hashlib.sha256(raw).hexdigest()!=expected['sha256']:raise ValueError('Qualified native image changed: '+basename)
    output.write(raw)
   if os.name!='nt':target.chmod(0o755)
   selected[basename]=expected['sha256']
 archive.unlink()
 # Exact scientific binaries are reused. Only the controller's Python closure
 # is refrozen; its generated image binder is recorded independently.
 cpu_source=out/'cpu-freeze-source';cpu_source.mkdir()
 cpu_inputs={}
 for path in sorted((root/'src/engine/cpu_auditor/shared_service').glob('*.py')):
  if path.name.startswith('test_'):continue
  shutil.copyfile(path,cpu_source/path.name);cpu_inputs[path.relative_to(root).as_posix()]=sha(path)
 suffix='.exe'if os.name=='nt'else'';names=('horizon-cpu-shared68-worker'+suffix,'horizon-cpu-inventory'+suffix)
 binder=cpu_source/'native_image_identity.py';binder.write_text('"""Frozen binding to exact qualified input images; no runtime override."""\nHASHES='+repr({os.name:tuple(selected[n]for n in names)})+'\n',encoding='utf-8')
 host=out/'host-build';run(sys.executable,root/'tools/build_horizon_engine_host.py','--output',host)
 cpu=out/'cpu-build';command=[sys.executable,'-m','PyInstaller','--noconfirm','--onedir','--console','--name','horizon-cpu-controller','--distpath',str(cpu/'dist'),'--workpath',str(cpu/'work'),'--specpath',str(cpu/'spec'),'--paths',str(cpu_source)]
 for name in names:command+=['--add-binary',str(native/name)+os.pathsep+'native']
 for excluded in ('numpy','torch','tensorflow','matplotlib','scipy','pandas'):command+=['--exclude-module',excluded]
 run(*command,cpu_source/'service.py')
 runtime=out/'runtime';shutil.copytree(host/'dist/horizon-engine-host',runtime/'EngineRuntime/network');shutil.copytree(cpu/'dist/horizon-cpu-controller',runtime/'EngineRuntime/cpu')
 versions=json.loads((root/'tools/component_versions.json').read_bytes());tests={}
 for kind,folder,program,version in [('host','network','horizon-engine-host',versions['gpu']),('cpu','cpu','horizon-cpu-controller',versions['cpu'])]:
  executable=runtime/'EngineRuntime'/folder/(program+suffix)
  result=subprocess.run([str(executable),'--self-test'],capture_output=True,timeout=30,check=True)
  value=json.loads(result.stdout)
  if value.get('PASS')is not True or value.get('version')!=version or value.get('frozen')is not True:raise ValueError('Frozen self-test identity failed: '+kind)
  for flag in ('network_requested','installation_performed','native_worker_started','scientific_promotion'):
   if value.get(flag)is not False:raise ValueError('Frozen self-test boundary failed: '+flag)
  (out/(kind.upper()+'-SELFTEST.json')).write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');tests[kind]=value
  result=subprocess.run([str(executable),'--version'],capture_output=True,timeout=15,check=True)
  if version not in result.stdout.decode():raise ValueError('Frozen version command differs')
 for name in names:
  if sha(runtime/'EngineRuntime/cpu/_internal/native'/name)!=selected[name]:raise ValueError('Frozen native image differs from qualified input')
 # Prove the reused native worker resolves its matching closure, without a
 # mining task, affinity change or arithmetic. This option exits before setup.
 worker=runtime/'EngineRuntime/cpu/_internal/native'/names[0]
 env=dict(os.environ)
 if os.name!='nt':env['LD_LIBRARY_PATH']=str(runtime/'EngineRuntime/cpu/_internal')+os.pathsep+env.get('LD_LIBRARY_PATH','')
 probe=subprocess.run([str(worker),'--component-info'],capture_output=True,timeout=15,check=True,env=env)
 native_info=json.loads(probe.stdout)
 if native_info.get('schema')!='PGN_CPU_SHARED_COMPONENT_V1'or native_info.get('production_admitted')is not False or native_info.get('canonical_source_manifest_sha256')!='96C600D3486DED57B400FCD4428E904F6A3743E49F587DC85E93847D9FFDDE13':raise ValueError('Reused native worker component identity differs')
 (out/'NATIVE-LOAD-PROBE.json').write_text(json.dumps(dict(PASS=True,component_info=native_info,tasks_sent=0,component_info_only=True),indent=2)+'\n',encoding='utf-8')
 files={p.relative_to(runtime).as_posix():dict(sha256=sha(p),bytes=p.stat().st_size)for p in sorted(runtime.rglob('*'))if p.is_file()}
 if any(sha(root/name)!=digest for name,digest in cpu_inputs.items()):raise ValueError('CPU source changed during freeze')
 cpu_manifest=dict(schema='PGN_CPU_CONTROLLER_SOURCE_V1',sources=cpu_inputs,generated_binder_sha256=sha(binder),native_inputs=selected,input_archive_sha256=inputs['archive_sha256'],component_version=versions['cpu'],executable_sha256=files['EngineRuntime/cpu/horizon-cpu-controller'+suffix]['sha256'],python=sys.version,scope='Controller/closure qualification only; native scientific images reused unchanged')
 (out/'CPU-SOURCE.json').write_text(json.dumps(cpu_manifest,indent=2)+'\n',encoding='utf-8');shutil.copy2(host/'ENGINE-HOST-SOURCE.json',out/'ENGINE-HOST-SOURCE.json');(out/'BINARY-FILES.json').write_text(json.dumps(files,indent=2)+'\n',encoding='utf-8')
 receipt=dict(PASS=True,platform=platform,source_commit=os.environ.get('GITHUB_SHA'),run=os.environ.get('GITHUB_RUN_ID'),native_inputs=selected,input_archive_sha256=inputs['archive_sha256'],input_descriptor_sha256=sha(descriptor),versions=dict(gpu=versions['gpu'],cpu=versions['cpu']),self_tests=tests,files=len(files),native_science_recompiled=False,network_used_during_selftest=False,installation_performed=False,live_profile_changed=False)
 (out/'BUILD-RECEIPT.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
 print(json.dumps(dict(PASS=True,platform=platform,versions=receipt['versions'],files=len(files),host_sha256=files['EngineRuntime/network/horizon-engine-host'+suffix]['sha256'],cpu_sha256=cpu_manifest['executable_sha256'])))
if __name__=='__main__':main()
