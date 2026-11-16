from pathlib import Path
import argparse,json,shlex,subprocess,sys,hashlib,time
p=argparse.ArgumentParser();p.add_argument('--targets',nargs='+',required=True);args=p.parse_args()
r=Path('C:/PGN/tests/social223/r246-functional');sdk=Path('C:/PGN/tests/social223/r246-consent-43760952649/artifact/sdk');out=r/'offchain-native-build';(out/'objects').mkdir(parents=True,exist_ok=True)
manifest=json.loads((sdk/'MANIFEST.json').read_text());assert manifest['source']=='9623bcc2410b010ad92df58d8e7bab47063767d1'
commands=json.loads((sdk/'build/compile_commands.json').read_text());fallback=next(x for x in commands if x['file'].replace('\\','/').endswith('/src/pgn/social.cpp'))
compiler=Path('C:/Gapcoin/toolchain/msys64/ucrt64/bin/g++.exe')
old=manifest['source_root'].replace('\\','/');build=manifest['build_root'].replace('\\','/')
receipts=[]
for target in args.targets:
 assert target in['pgn/social_display_native.cpp','pgn/social.cpp','rpc/social.cpp','init.cpp','net_processing.cpp','rpc/pgn_native.cpp']
 source=r/'offchain-native-source/src'/target;entry=next((x for x in commands if x['file'].replace('\\','/').endswith('/src/'+target)),fallback)
 tokens=[x.strip('"').replace('\\','/')for x in shlex.split(entry['command'],posix=False)];flags=[]
 for arg in tokens[1:]:
  if arg=='-o':break
  flags.append(arg.replace(old,str(sdk/'source').replace('\\','/')).replace(build,str(sdk/'build').replace('\\','/')))
 object=out/'objects'/(target.replace('/','_')+'.obj')
 command=[str(compiler),'-I'+str(r/'offchain-native-source/src'),'-I'+str(sdk/'thirdparty/include')]+flags+['-o',str(object),'-c',str(source)]
 receipt=dict(overlay_manifest_sha256=hashlib.sha256((r/'OFFCHAIN-NATIVE-SOURCE-INPUTS.json').read_bytes()).hexdigest(),target=target,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),command=command,sdk_source=manifest['source'],compiler=subprocess.check_output([str(compiler),'--version'],text=True).splitlines()[0])
 (out/(target.replace('/','_')+'-command.json')).write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf8')
 start=time.monotonic();code=subprocess.call(command,cwd=out);receipt.update(exit_code=code,elapsed=time.monotonic()-start)
 if code==0:receipt['object_sha256']=hashlib.sha256(object.read_bytes()).hexdigest()
 (out/(target.replace('/','_')+'-result.json')).write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf8');receipts.append(receipt)
 print(json.dumps(dict(target=target,exit_code=code,elapsed=receipt['elapsed'])),flush=True)
 if code:raise SystemExit(code)

