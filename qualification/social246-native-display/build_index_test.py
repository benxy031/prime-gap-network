from pathlib import Path
import json,subprocess,os,hashlib,time
r=Path('C:/PGN/tests/social223/r246-functional');out=r/'index-test-build';out.mkdir(exist_ok=True);(out/'tmp').mkdir(exist_ok=True)
recipe=json.loads((r/'indexed-native-build/pgn_social_display_native.cpp-command.json').read_text())
command=recipe['command'][:recipe['command'].index('-o')]+['-DPGN_SOCIAL_DISPLAY_INDEX_TESTING=1','-o',str(out/'index_store_test.obj'),'-c',str(r/'index_store_test.cpp')]
env=os.environ.copy();env.update(TEMP=str(out/'tmp'),TMP=str(out/'tmp'),TMPDIR=str(out/'tmp'))
start=time.monotonic();code=subprocess.call(command,cwd=out,env=env)
(out/'COMPILE.json').write_text(json.dumps(dict(command=command,exit_code=code,elapsed=time.monotonic()-start),indent=2)+'\n',encoding='utf8')
if code:raise SystemExit(code)
base=json.loads((r/'counts-safe-build/LINK-RECIPE.json').read_text())['link_command']
link=[base[0],str(out/'index_store_test.obj')]+[arg for arg in base[1:]if not arg.endswith('.obj')]
link[link.index('-o')+1]=str(out/'pgn-social-index-store-test.exe')
for i,arg in enumerate(link):
 if arg.startswith('-Wl,--out-implib,'):link[i]='-Wl,--out-implib,'+str(out/'index-test.dll.a')
start=time.monotonic();code=subprocess.call(link,cwd=out,env=env)
(out/'LINK.json').write_text(json.dumps(dict(command=link,exit_code=code,elapsed=time.monotonic()-start),indent=2)+'\n',encoding='utf8')
if code:raise SystemExit(code)
for name in ['libgmp-10.dll','libsqlite3-0.dll']:
 if not (out/name).exists():os.link(r/'counts-safe-build/Core'/name,out/name)
print(json.dumps(dict(exe=str(out/'pgn-social-index-store-test.exe'),sha256=hashlib.sha256((out/'pgn-social-index-store-test.exe').read_bytes()).hexdigest())))
