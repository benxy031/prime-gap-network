"""QA-only stdin bridge to real isolated Native wallets. Never distributed."""
from pathlib import Path
import base64,http.client,json,sys,time,urllib.parse,uuid,subprocess
from authority import Store,Verifier,Denied,Conflict,Unavailable,DOMAIN,canonical
from access_backend import Backend
config=json.loads(Path(sys.argv[1]).read_bytes());request=json.loads(sys.stdin.buffer.read(65537));actor=config['actors'][request.get('actor','wizz')]
def rpc(method,args=[],wallet=None):
 auth=base64.b64encode(Path(config['cookie']).read_bytes().strip()).decode();c=http.client.HTTPConnection('127.0.0.1',config['rpc'],timeout=10);path='/'if wallet is None else'/wallet/'+urllib.parse.quote(wallet,safe='');c.request('POST',path,json.dumps(dict(jsonrpc='2.0',id='access278-qt',method=method,params=args)),{'Authorization':'Basic '+auth,'Content-Type':'application/json'});v=json.loads(c.getresponse().read(262144));c.close()
 if v.get('error'):raise Unavailable(str(v['error']))
 return v['result']
def verify(script,address,message,signature):return rpc('getaddressinfo',[address],actor['wallet'])['scriptPubKey']==script and rpc('verifymessage',[address,signature,message])is True
try:
 action=request['action'];args=request.get('args',{});revision=int(request.get('revision',0))
 body=dict(v=1,genesis=config['genesis'],community=config['community'],request_id=request.get('request_id',str(uuid.uuid4())),actor=actor['script'],client=actor['client'],address=actor['address'],created_ms=int(time.time()*1000),revision=revision,action=action,args=args)
 message=request.get('wire',DOMAIN+canonical(body))
 if not isinstance(message,str)or not message.startswith(DOMAIN)or len(message.encode())>16384:raise Denied('QA access domain only')
 wire=json.loads(message[len(DOMAIN):])
 if any(wire.get(key)!=body[key]for key in ['genesis','community','actor','client','address']):raise Denied('QA signing context differs')
 if config.get('signer_script'):
  payload=dict(genesis=config['genesis'],script=actor['script'],client=actor['client'],community=config['community'],rpc_port=config['rpc'],cookie=config['cookie'],wallet=actor['wallet'],miner_address=actor['address'],challenge=message)
  encoded=base64.b64encode(Path(config['signer_script']).read_text(encoding='utf-8').encode('utf-16le')).decode()
  r=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-EncodedCommand',encoded],input=(json.dumps(payload)+'\n').encode(),capture_output=True,timeout=20,creationflags=subprocess.CREATE_NO_WINDOW)
  reply=json.loads(r.stdout.decode('utf-8-sig').strip())
  if r.returncode or reply.get('schema')!='PGN_REMOTE_ACCESS_SIGNED_1'or reply.get('genesis')!=config['genesis']or reply.get('script')!=actor['script']or not reply.get('signature'):raise Unavailable(reply.get('error','Scoped Native signer refused'))
  signature=reply['signature']
 else:signature=rpc('signmessage',[actor['address'],message],actor['wallet'])
 if action=='sign_only':print(json.dumps(dict(ok=True,signature=signature)),flush=True);raise SystemExit(0)
 c=http.client.HTTPConnection('127.0.0.1',config['http_port'],timeout=10);c.request('POST','/v1/access',json.dumps(dict(message=message,signature=signature)),{'Content-Type':'application/json'});response=c.getresponse();raw=response.read(1048577);c.close()
 if len(raw)>1048576:raise Unavailable('Response budget exceeded')
 value=json.loads(raw);print(json.dumps(value),flush=True)
except(Denied,Conflict,Unavailable)as e:print(json.dumps(dict(ok=False,error=type(e).__name__,reason=str(e))),flush=True)
