"""Export actual PE closure BEFORE tests; standard SDK imports, never wallets."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--tool-bin',required=True);a=p.parse_args();tool=Path(a.tool_bin);out=Path('private-social223-delivery');stage=out/'Core';stage.mkdir(parents=True,exist_ok=True)
exe=stage/'pgn-core.exe';shutil.copy2('build-native/bin/riecoind.exe',exe);queue=[exe];seen=set();system=[]
while queue:
 binary=queue.pop();text=subprocess.check_output([str(tool/'objdump.exe'),'-p',str(binary)],text=True,encoding='utf-8')
 for name in re.findall(r'DLL Name:\s*(\S+)',text):
  if name.lower()in seen:continue
  seen.add(name.lower());source=tool/name
  if source.is_file():target=stage/name;shutil.copy2(source,target);queue.append(target)
  else:system.append(name)
files={f.name:dict(bytes=f.stat().st_size,sha256=hashlib.sha256(f.read_bytes()).hexdigest())for f in stage.iterdir()if f.is_file()}
env=os.environ.copy();env['PATH']=os.environ['SystemRoot']+'/System32;'+os.environ['SystemRoot']
run=subprocess.run([str(exe.resolve()),'--version'],env=env,capture_output=True,text=True,timeout=15)
help_run=subprocess.run([str(exe.resolve()),'--help'],env=env,capture_output=True,text=True,timeout=15)
cache=Path('build-native/CMakeCache.txt').read_text(encoding='utf-8');mode=re.search(r'^PGN_SOCIAL_PRIVATE_QUALIFICATION:BOOL=(\S+)$',cache,re.M);assert mode,'Qualification option missing from configured cache'
(out/'PRETEST-CORE-EXPORT.json').write_text(json.dumps(dict(files=files,qualification_build_option=mode.group(1),system_imports=sorted(system),version_exit=run.returncode,version_output=run.stdout,version_error=run.stderr,help_exit=help_run.returncode,help_output=help_run.stdout,help_error=help_run.stderr,scope='Private synthetic moderation qualification, no public activation'),indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(closure_files=len(files),version_exit=run.returncode,version_output=run.stdout[:300])))
if run.returncode:raise SystemExit(run.returncode)
if help_run.returncode:raise SystemExit(help_run.returncode)
