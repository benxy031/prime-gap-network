from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--core',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--sdk',type=Path);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);shutil.copyfile(a.core,a.out/a.core.name);(a.out/a.core.name).chmod(a.core.stat().st_mode)
if os.name=='nt':
 assert a.sdk and a.sdk.is_dir();pending=[a.out/a.core.name];seen=set()
 while pending:
  f=pending.pop();text=subprocess.check_output([str(a.sdk/'objdump.exe'),'-p',str(f)],text=True,encoding='utf-8',errors='replace')
  for name in re.findall(r'^\s*DLL Name:\s*(\S+)\s*$',text,re.M):
   if name.lower()in seen:continue
   seen.add(name.lower());source=a.sdk/name
   if source.is_file():target=a.out/name;shutil.copyfile(source,target);target.chmod(source.stat().st_mode);pending.append(target)
   elif name.lower().startswith(('api-ms-win-','ext-ms-win-'))or(Path(os.environ['SystemRoot'])/'System32'/name).is_file():continue
   else:raise RuntimeError('Unresolved production runtime '+name)
files={f.name:dict(bytes=f.stat().st_size,sha256=hashlib.sha256(f.read_bytes()).hexdigest())for f in sorted(a.out.iterdir())if f.is_file()}
print(json.dumps(dict(source=os.environ.get('GITHUB_SHA'),version='2026.11.17.31',files=files)))
