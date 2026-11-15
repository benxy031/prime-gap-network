from pathlib import Path
import subprocess,json,hashlib,sys
r=Path.cwd();out=r/'private-social223-delivery/components';out.mkdir(parents=True,exist_ok=True)
results={}
for name in ['consent-prepare','consent-pending','committed-identity']:
 exe=r/'build-native/bin'/('pgn-social-'+name+'-test.exe')
 if not exe.exists():exe=r/'build-native/src'/exe.name
 if not exe.exists():raise SystemExit('Built component not found: '+exe.name)
 args=[str(exe)]
 if name!='identity-batch':args.append(str(r/('private-association-'+name)))
 run=subprocess.run([sys.executable,'tools/social223_bounded_windows.py','--cwd',str(r),'--receipt',str(out/(name+'-resource.json')),'--timeout','90','--',*args],capture_output=True)
 (out/(name+'.stdout')).write_bytes(run.stdout);(out/(name+'.stderr')).write_bytes(run.stderr)
 results[name]=dict(exit_code=run.returncode,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
 print(name,run.returncode,run.stdout.decode(errors='replace')[-300:])
 if run.returncode:break
(out/'RESULT.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf8');sys.exit(any(x['exit_code'] for x in results.values()))
