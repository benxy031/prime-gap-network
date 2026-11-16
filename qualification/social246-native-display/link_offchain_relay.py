from pathlib import Path
import json, subprocess, hashlib, os, shutil, re, time
r=Path('C:/PGN/tests/social223/r246-functional')
base=r/'counts-safe-build'; out=r/'offchain-relay-build'
assert not (out/'MANIFEST.json').exists(), 'Immutable successor already exists'
(out/'objects').mkdir(parents=True,exist_ok=True); (out/'tmp').mkdir(exist_ok=True)
code=out/'Core'; code.mkdir(exist_ok=True)
sdk=Path('C:/PGN/tests/social223/r246-consent-43760952649/artifact/sdk')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
env=os.environ.copy(); env.update(TEMP=str(out/'tmp'),TMP=str(out/'tmp'),TMPDIR=str(out/'tmp'))
recipe=json.loads((base/'LINK-RECIPE.json').read_text()); link=recipe['link_command'][:]
changed=['pgn/social.cpp','rpc/social.cpp','init.cpp','net_processing.cpp','rpc/pgn_native.cpp']
matched=0
for target in changed:
 old_obj=str(base/'objects'/(target.replace('/','_')+'.obj')).replace('\\','/')
 new_obj=r/'offchain-native-build/objects'/(target.replace('/','_')+'.obj')
 assert new_obj.is_file(),target
 for i,arg in enumerate(link):
  if arg.replace('\\','/')==old_obj:link[i]=str(new_obj);matched+=1
assert matched==len(changed)
extra=r/'offchain-native-build/objects/pgn_social_display_native.cpp.obj';assert extra.is_file();link.insert(1,str(extra))
for i,arg in enumerate(link):
 if arg.startswith('-Wl,--out-implib,'):link[i]='-Wl,--out-implib,'+str(out/'libriecoind.dll.a')
link[link.index('-o')+1]=str(code/'pgn-core.exe')
inputs={arg:sha(Path(arg)) for arg in link if arg.endswith(('.obj','.a')) and Path(arg).is_file()}
start=time.monotonic(); p=subprocess.run(link,cwd=out,env=env)
link_result=dict(command=link,exit_code=p.returncode,elapsed=time.monotonic()-start,input_sha256=inputs)
(out/'LINK.json').write_text(json.dumps(link_result,indent=2)+'\n',encoding='utf8')
if p.returncode:raise SystemExit(p.returncode)
for name in ['libgmp-10.dll','libsqlite3-0.dll']:
 dst=code/name
 if not dst.exists():os.link(base/'Core'/name,dst)
assert sha(base/'Core/pgn-core.exe')=='4abbd63c97d12f8a314bc453c9196cb328dc0ef8396ac98efdb04aacca83f5fe'
probe_env=env.copy();probe_env['PATH']=os.environ['SystemRoot']+'/System32;'+os.environ['SystemRoot']
probes={}
for arg in ['--version','--help']:
 p=subprocess.run([str(code/'pgn-core.exe'),arg],cwd=code,env=probe_env,capture_output=True,timeout=20)
 probes[arg]=dict(exit_code=p.returncode,stdout=p.stdout.decode('utf8','replace')[:2500],stderr=p.stderr.decode('utf8','replace')[-1200:]);assert p.returncode==0
assert '2026.11.15.22' in probes['--version']['stdout']
tool=Path('C:/Gapcoin/toolchain/msys64/ucrt64/bin/objdump.exe')
imports=re.findall(r'DLL Name:\s*(\S+)',subprocess.check_output([str(tool),'-p',str(code/'pgn-core.exe')],text=True))
manifest=dict(source='43a02d891a46e4944928d8fda9029261f3c85181',base_source='7ee2a595c95708353ae813ff3797e53bb853a79c',scope='PRIVATE current-display relay checkpoint; Root safe Core22 unchanged; no public activation, no old inclusion worker',baseline_exe_preserved=True,recompiled_tus=changed+['pgn/social_display_native.cpp'],resource_reused_sha256=sha(base/'objects/riecoind-res22.rc.obj'),files={p.name:dict(bytes=p.stat().st_size,sha256=sha(p))for p in code.iterdir()if p.is_file()},probes=probes,imports=imports,compile_exit=0,link_exit=p.returncode,sdk_source=recipe['sdk_source'],sdk_manifest_sha256=recipe['sdk_manifest_sha256'],runtime_activated=False,wallets_copied=False,actual_two_peer_qualification_pending=True)
(out/'MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8');print(json.dumps(manifest))
