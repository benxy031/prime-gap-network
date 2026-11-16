"""Loopback-only community service; deployment supplies an explicit TLS edge."""
from pathlib import Path
import argparse,base64,contextlib,http.client,json,threading,time,urllib.parse
from authority import Store,Verifier,Unavailable
from access_backend import Backend
from access_http import AccessServer
from community_keys import CommunityKeys
from content_index import ContentIndex,projection

class CoreReader:
 def __init__(self,config):self.config=config;self.slots=threading.BoundedSemaphore(1)
 def __call__(self,method,args):
  if method not in{'getblockhash','validateaddress','verifymessage','getsocialevents'}:raise Unavailable('RPC purpose refused')
  if not self.slots.acquire(timeout=.25):raise Unavailable('Core read budget busy')
  try:
   cookie=Path(self.config['cookie']).read_bytes().strip()
   if len(cookie)>1024:raise Unavailable('Invalid Core cookie')
   c=http.client.HTTPConnection('127.0.0.1',self.config['rpc_port'],timeout=3)
   try:
    c.request('POST','/',json.dumps(dict(jsonrpc='2.0',id='community-read',method=method,params=args)),{'Authorization':'Basic '+base64.b64encode(cookie).decode(),'Content-Type':'application/json'});r=c.getresponse();raw=r.read(262145)
    if r.status!=200 or len(raw)>262144:raise Unavailable('Core response unavailable')
    value=json.loads(raw)
    if value.get('error'):raise Unavailable('Core rejected the public proof request')
    return value['result']
   finally:c.close()
  finally:self.slots.release()

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--config',required=True,type=Path);args=parser.parse_args();raw=args.config.read_bytes()
 if len(raw)>16384:raise ValueError('Bounded explicit community configuration required')
 config=json.loads(raw);database=Path(config['database']);database.parent.mkdir(parents=True,exist_ok=True);rpc=CoreReader(config)
 if rpc('getblockhash',[0])!=config['genesis']:raise ValueError('Core genesis mismatch')
 registry=CommunityKeys(rpc,config['genesis']);bootstrap=Store(database,config['genesis'],config['moderators'],create=True,account_registry=registry.resolve);community=bootstrap.community;bootstrap.close()
 index=ContentIndex(database,config['genesis'],rpc);stopping=threading.Event()
 def backend():
  keys=CommunityKeys(rpc,config['genesis']);store=Store(database,config['genesis'],config['moderators'],content_lookup=index.lookup,account_registry=keys.resolve);return Backend(store,Verifier(config['genesis'],keys.verify_and_bind,require_client=True),config.get('labels'))
 def index_loop():
  while not stopping.is_set():
   try:index.tick()
   except Exception:pass # An index outage disables unknown targets; it grants nothing.
   stopping.wait(15)
 worker=threading.Thread(target=index_loop,daemon=True);worker.start()
 server=AccessServer(('127.0.0.1',config['listen_port']),backend,projection=lambda q:projection(database,config['genesis'],community,q));server.authorization_slots=threading.BoundedSemaphore(1)
 print(json.dumps(dict(ready=True,community=community,genesis=config['genesis'],loopback_port=config['listen_port'])),flush=True)
 try:server.serve_forever(poll_interval=.5)
 finally:stopping.set();server.server_close();worker.join(timeout=4)
if __name__=='__main__':main()
