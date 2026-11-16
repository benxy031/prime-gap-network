from pathlib import Path
import concurrent.futures,http.client,json,os,subprocess,sys,tempfile,time,unittest,urllib.parse,uuid
from authority import DOMAIN,canonical

class StartupTests(unittest.TestCase):
 def test_service_starts_without_core_and_keeps_privileged_access_unavailable(self):
  with tempfile.TemporaryDirectory()as folder:
   root=Path(folder);config=dict(database=str(root/'community.sqlite3'),cookie=str(root/'absent-cookie'),rpc_port=1,listen_port=0,genesis='7'*64,moderators=['1'*64]);path=root/'config.json';path.write_text(json.dumps(config))
   flags=subprocess.CREATE_NO_WINDOW if os.name=='nt'else 0
   p=subprocess.Popen([sys.executable,'-u',str(Path(__file__).with_name('serve.py')),'--config',str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=flags)
   try:
    with concurrent.futures.ThreadPoolExecutor(max_workers=1)as pool:ready=json.loads(pool.submit(p.stdout.readline).result(timeout=8))
    self.assertTrue(ready['ready']);self.assertIsNone(p.poll());port=ready['loopback_port']
    c=http.client.HTTPConnection('127.0.0.1',port,timeout=3);c.request('GET','/v1/projection?'+urllib.parse.urlencode(dict(community=ready['community'],channel='global',targets='')));reply=c.getresponse();v=json.loads(reply.read());c.close();self.assertEqual(reply.status,200);self.assertEqual(v['value']['removed'],[])
    command=dict(v=1,genesis=config['genesis'],community=ready['community'],request_id=str(uuid.uuid4()),actor='0014'+'2'*40,client='0014'+'2'*40,address='FixtureAddress12345678',created_ms=int(time.time()*1000),revision=0,action='read_access',args=dict(channel='global',member='',role=''))
    c=http.client.HTTPConnection('127.0.0.1',port,timeout=3);c.request('POST','/v1/access',json.dumps(dict(message=DOMAIN+canonical(command),signature='fixture')),{"Content-Type":"application/json"});reply=c.getresponse();v=json.loads(reply.read());c.close();self.assertEqual(reply.status,503);self.assertFalse(v['ok'])
   finally:
    p.terminate();p.communicate(timeout=5)
if __name__=='__main__':unittest.main()
