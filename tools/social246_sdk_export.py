"""Exact private Native link SDK, exported before tests. No wallets/data/config."""
from pathlib import Path
import subprocess,shutil,hashlib,json,re
src=Path('wallet/riecoin-core-upstream-v2606').resolve();build=Path('build-native').resolve();out=Path('private-social223-delivery/sdk');out.mkdir(parents=True,exist_ok=True);files={}
def copy(p,name):
 dest=out/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dest)
 files[name]=dict(bytes=dest.stat().st_size,sha256=hashlib.sha256(dest.read_bytes()).hexdigest())
for p in build.rglob('*.a'):copy(p,'build/'+p.relative_to(build).as_posix())
for p in build.rglob('*.obj'):
 if any('riecoind.dir'==part for part in p.parts):copy(p,'build/'+p.relative_to(build).as_posix())
for p in build.rglob('*.h'):copy(p,'build/'+p.relative_to(build).as_posix())
for p in src.rglob('*'):
 if p.is_file()and p.suffix in ('.h','.hpp','.ipp','.tpp'):copy(p,'source/'+p.relative_to(src).as_posix())
for name in ['compile_commands.json','CMakeCache.txt']:
 p=build/name;assert p.stat().st_size<8*1024*1024;copy(p,'build/'+name)
commands=json.loads((build/'compile_commands.json').read_text(encoding='utf8'));wanted=[x for x in commands if x['file'].replace('\\','/').endswith(('/src/pgn/social_identity_rpc.cpp','/src/rpc/pgn_native.cpp','/src/pgn/social.cpp','/src/init.cpp'))]
assert len(wanted)==4
# Include exact external library headers reached by these objects; reuse the
# existing compiler/system headers instead of copying another toolchain.
tool=Path(shutil.which('ninja'));assert tool.is_file()
deps=subprocess.Popen([str(tool),'-C',str(build),'-t','deps'],stdout=subprocess.PIPE,text=True,encoding='utf8',errors='replace')
external=set();msys=Path(shutil.which('g++')).parent.parent;inc=(msys/'include').resolve()
for line in deps.stdout:
 text=line.strip()
 if not line.startswith('    ')or not text:continue
 p=Path(text)
 if not p.is_absolute():p=build/p
 if not p.is_file():continue
 try:relative=p.resolve().relative_to(inc)
 except ValueError:continue
 if relative.parts[0]=='boost'or relative.as_posix()in ['gmp.h','sqlite3.h']or relative.parts[0]=='event2':external.add(p.resolve())
assert deps.wait()==0
for p in sorted(external):copy(p,'thirdparty/include/'+p.relative_to(inc).as_posix())
commands_run=subprocess.Popen([str(tool),'-C',str(build),'-t','commands','riecoind'],stdout=subprocess.PIPE,text=True,encoding='utf8',errors='replace')
last=''
for line in commands_run.stdout:
 if re.search(r'-o\s+\"?bin[\\\\/]+riecoind[.]exe',line):last=line.rstrip()
assert commands_run.wait()==0 and last,'Exact daemon link command missing'
for name in re.findall(r'[A-Za-z]:[\\/][^\s";]+[.]a',last):
 p=Path(name)
 if p.is_file()and msys.resolve()in p.resolve().parents:copy(p,'thirdparty/lib/'+p.name)
if (src/'COPYING').is_file():copy(src/'COPYING','COPYING.Native')
(out/'link-command.txt').write_text(last+'\n',encoding='utf8');(out/'affected-commands.json').write_text(json.dumps(wanted,indent=2)+'\n',encoding='utf8')
git=shutil.which('git')or'C:/Program Files/Git/bin/git.exe';pin=subprocess.check_output([git,'rev-parse','HEAD'],text=True).strip()
cache=(build/'CMakeCache.txt').read_text(encoding='utf8');assert 'PGN_SOCIAL_PRIVATE_QUALIFICATION:BOOL=OFF'in cache
manifest=dict(source=pin,qualification_option='OFF',source_root=str(src),build_root=str(build),toolchain_root=str(msys),files=files,link_sha256=hashlib.sha256((out/'link-command.txt').read_bytes()).hexdigest(),commands_sha256=hashlib.sha256((out/'affected-commands.json').read_bytes()).hexdigest(),bytes=sum(x['bytes']for x in files.values()),limits=['Private SDK; no public release or runtime installation','Compiler/system headers are not duplicated; compiler/ABI compatibility must be checked before a local relink','No wallet, cookie, RPC credential, test datadir or mining runtime is exported'])
(out/'MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8');print(json.dumps(dict(source=pin,files=len(files),bytes=manifest['bytes'],archives=sum(name.endswith('.a')for name in files),export='PASS')))
