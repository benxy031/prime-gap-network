"""Actual Core20 on-disk fixture -> ordinary Core25 startup, no repair flags."""
from pathlib import Path,PurePosixPath
import argparse,base64,hashlib,http.client,json,os,socket,subprocess,time,zipfile
p=argparse.ArgumentParser();p.add_argument('--core',type=Path,required=True);p.add_argument('--fixture',type=Path,required=True);p.add_argument('--work',type=Path,required=True);p.add_argument('--receipt',type=Path,required=True);p.add_argument('--optional-check-level',type=int,choices=range(5));a=p.parse_args();a.core=a.core.resolve();a.work.mkdir(parents=True,exist_ok=False)
with zipfile.ZipFile(a.fixture)as z:
 assert len(z.infolist())<256 and sum(i.file_size for i in z.infolist())<128*1024*1024
 for item in z.infolist():
  rel=PurePosixPath(item.filename);assert not rel.is_absolute()and'..'not in rel.parts and':'not in item.filename and'\\'not in item.filename and'wallets'not in rel.parts
  to=a.work/rel;to.parent.mkdir(parents=True,exist_ok=True);to.write_bytes(z.read(item))
fixture=json.loads((a.work/'FIXTURE.json').read_bytes());assert fixture['no_wallets']and fixture['public_data_only']
for name,sha in fixture['files'].items():assert hashlib.sha256((a.work/name).read_bytes()).hexdigest()==sha
def free():
 s=socket.socket();s.bind(('127.0.0.1',0));port=s.getsockname()[1];s.close();return port
rows=[]
for cycle in ['migration','cold-restart']:
 port=free();cookie=a.work/'testnet/.cookie';assert not cookie.exists();log=a.work/'testnet/debug.log';before=log.stat().st_size if log.exists()else 0
 args=[str(a.core),'-datadir='+str(a.work.resolve()),'-testnet=1','-server=1','-rpcbind=127.0.0.1','-rpcallowip=127.0.0.1','-rpcport='+str(port),'-port='+str(free()),'-disablewallet=1','-listen=0','-connect=0','-dnsseed=0','-fixedseeds=0','-discover=0','-par=1','-dbcache=64']
 if a.optional_check_level is not None:args.append('-checklevel='+str(a.optional_check_level))
 assert not any(x.startswith('-reindex')for x in args)
 assert [x for x in args if x.startswith('-checklevel')]==([]if a.optional_check_level is None else ['-checklevel='+str(a.optional_check_level)])
 env=dict(os.environ)
 if os.name=='nt':env['PATH']=os.environ['SystemRoot']+'/System32;'+os.environ['SystemRoot']
 out=(a.work/(cycle+'.stdout')).open('wb');err=(a.work/(cycle+'.stderr')).open('wb');started=time.monotonic();process=subprocess.Popen(args,stdout=out,stderr=err,env=env,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));out.close();err.close()
 def rpc(method,params=[]):
  auth=base64.b64encode(cookie.read_bytes().strip()).decode();c=http.client.HTTPConnection('127.0.0.1',port,timeout=15);c.request('POST','/',json.dumps(dict(jsonrpc='2.0',id='core25-legacy',method=method,params=params)),{'Authorization':'Basic '+auth,'Content-Type':'application/json'});value=json.loads(c.getresponse().read(100000));c.close();assert value.get('error')is None,value.get('error');return value['result']
 try:
  deadline=time.monotonic()+150;chain=None
  while time.monotonic()<deadline and process.poll()is None:
   try:
    observed=rpc('getblockchaininfo')
    if observed['blocks']==fixture['canonical_height']:chain=observed;break
   except Exception:pass
   time.sleep(.2)
  assert chain is not None,('Ordinary Core startup failed',process.poll())
  assert rpc('getblockhash',[0])==fixture['genesis']and chain['bestblockhash']==fixture['canonical_tip']
  assert rpc('verifychain',[4,50])is True;coins=rpc('gettxoutsetinfo',['muhash']);assert coins['muhash']==fixture['canonical_muhash']
  version=rpc('getnetworkinfo')['subversion'];assert '2026.11.16.25'in version
  with log.open('rb')as f:f.seek(before);newlog=f.read(16*1024*1024).decode('utf-8',errors='replace')
  count=newlog.count('Upgrading legacy PGN audit chainstate through complete block replay');assert count==(1 if cycle=='migration'else 0),(cycle,count)
  rows.append(dict(cycle=cycle,seconds=time.monotonic()-started,height=chain['blocks'],tip=chain['bestblockhash'],muhash=coins['muhash'],utxos=coins['txouts'],automatic_rebuilds=count,version=version,verifychain4_50=True,no_repair_arguments=True,optional_check_level=a.optional_check_level))
 finally:
  if process.poll()is None:
   try:rpc('stop')
   except Exception:pass
  process.wait(timeout=30)
  assert process.returncode==0,process.returncode
result=dict(PASS=True,core_sha256=hashlib.sha256(a.core.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(a.fixture.read_bytes()).hexdigest(),fixture_producer=fixture['producer_core20_sha256'],rows=rows,scope='Real Core20 database; normal Core25 startup and cold restart, no external repair/diagnostic tool, same full UTXO set at canonical tip; wallets disabled in this component test')
a.receipt.parent.mkdir(parents=True,exist_ok=True);a.receipt.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
