"""Collect an isolated Core20 executable/closure and exact Git input evidence."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,sys
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,default=Path('core233'));parser.add_argument('--tool-bin',type=Path);parser.add_argument('--git',default='git');args=parser.parse_args()
root=Path.cwd();out=args.output;stage=out/'Core';stage.mkdir(parents=True,exist_ok=True)
original=Path('build-native/bin')/('riecoind.exe'if os.name=='nt'else'riecoind')
target=stage/('pgn-core.exe'if os.name=='nt'else'pgn-core');shutil.copy2(original,target)
dependencies=[]
if os.name=='nt':
 assert args.tool_bin and args.tool_bin.is_dir()
 objdump=args.tool_bin/'objdump.exe';queue=[target];seen=set()
 while queue:
  binary=queue.pop();text=subprocess.check_output([str(objdump),'-p',str(binary)],text=True,encoding='utf-8')
  for name in re.findall(r'DLL Name:\s*(\S+)',text):
   if name.lower()in seen:continue
   seen.add(name.lower());source=args.tool_bin/name
   if source.is_file():
    destination=stage/name;shutil.copy2(source,destination);queue.append(destination)
   else:dependencies.append(name)
else:
 text=subprocess.check_output(['ldd',str(target)],text=True);assert'not found'not in text
 for name,path in re.findall(r'^\s*(\S+) => (/\S+)',text,re.M):
  if name.startswith(('libevent','libgmp','libsqlite','libstdc++','libgcc_s')):shutil.copy2(path,stage/name)
  else:dependencies.append(name)
version=subprocess.check_output([str(target.resolve()),'--version'],text=True)
assert'2026.11.15.20'in version
files={p.relative_to(out).as_posix():dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())for p in sorted(stage.iterdir())if p.is_file()}
(out/'CORE-EXPORT.json').write_text(json.dumps(dict(source=os.environ.get('GITHUB_SHA'),files=files,version_output=version),indent=2)+'\n',encoding='utf-8')
records=subprocess.check_output([args.git,'ls-tree','-rz','HEAD','wallet/riecoin-core-upstream-v2606']).split(b'\0')
inputs={}
for record in records:
 if not record:continue
 meta,name=record.split(b'\t',1);mode,kind,blob=meta.split();name=name.decode('utf-8')
 if kind!=b'blob':continue
 path=root/name;raw=os.readlink(path).encode()if path.is_symlink()else path.read_bytes()
 assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()==blob.decode(),'Core source bytes differ: '+name
 inputs[name]=hashlib.sha256(raw).hexdigest()
receipt=dict(schema='PGN_PRIVATE_CORE_COMPONENT_SOURCE_V1',source=subprocess.check_output([args.git,'rev-parse','HEAD'],text=True).strip(),component_version='2026.11.15.20',inputs=inputs,files=files,system_dependencies=sorted(dependencies),version_output=version,scope='Private Core RPC qualification component; no main/release/runtime activation')
(out/'CORE-SOURCE.json').write_text(json.dumps(receipt,sort_keys=True,separators=(',',':'))+'\n',encoding='utf-8')
print(json.dumps(dict(source=receipt['source'],version=receipt['component_version'],source_files=len(inputs),closure_files=len(files),executable_sha256=files[target.relative_to(out).as_posix()]['sha256'])))
