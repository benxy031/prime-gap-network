import http.client,json,threading,time,unittest
from access_http import AccessServer,MAX_BODY
from authority import Denied,Conflict

class HTTPTests(unittest.TestCase):
 def setUp(self):
  self.calls=0;self.closes=0;self.block=threading.Event();self.waiting=0;self.lock=threading.Lock();owner=self
  class Fake:
   store=None
   def __init__(self):self.store=self
   def close(self):owner.closes+=1
   def handle(self,message,signature):
    owner.calls+=1
    if signature!='fixture':raise Denied('Native proof refused')
    if message=='conflict':raise Conflict('Current revision changed')
    if message=='wait':
     with owner.lock:owner.waiting+=1
     owner.block.wait(3)
    return dict(fixture=True)
  self.server=AccessServer(('127.0.0.1',0),Fake,requests_per_second=100,burst=100);self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
 def tearDown(self):self.block.set();self.server.shutdown();self.server.server_close();self.thread.join(3)
 def request(self,method='POST',path='/v1/access',body=None,headers=None):
  if body is None:body=json.dumps(dict(message='valid',signature='fixture'))
  c=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=4);c.request(method,path,body,headers or{'Content-Type':'application/json'});r=c.getresponse();value=(r.status,json.loads(r.read()),dict(r.getheaders()));c.close();return value
 def test_no_network_exposure_by_default(self):
  with self.assertRaises(ValueError):AccessServer(('0.0.0.0',0),lambda:None)
 def test_methods_paths_and_budgets_before_authorization(self):
  self.assertEqual(self.request(method='GET')[0],405);self.assertEqual(self.request(path='/rpc')[0],404)
  self.assertEqual(self.request(body='{}',headers={'Content-Type':'application/json','Content-Length':str(MAX_BODY+1)})[0],413)
  self.assertEqual(self.calls,0)
 def test_closed_envelope_and_proof_refusal(self):
  self.assertEqual(self.request(body='[]')[0],403);self.assertEqual(self.request(body='not JSON')[0],403)
  self.assertEqual(self.request(body=json.dumps(dict(message='valid',signature='wrong')))[0],403);self.assertEqual(self.closes,1)
 def test_result_conflict_and_connection_close(self):
  status,value,headers=self.request();self.assertEqual(status,200);self.assertTrue(value['ok']);self.assertEqual(headers['Connection'],'close');self.assertEqual(headers['Cache-Control'],'no-store')
  self.assertEqual(self.request(body=json.dumps(dict(message='conflict',signature='fixture')))[0],409);self.assertEqual(self.closes,2)
 def test_duplicate_length_and_transfer_encoding_refused(self):
  for transfer in [False,True]:
   c=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=4);c.putrequest('POST','/v1/access');c.putheader('Content-Type','application/json');c.putheader('Content-Length','2');c.putheader('Transfer-Encoding','chunked')if transfer else c.putheader('Content-Length','2');c.endheaders();r=c.getresponse();self.assertEqual(r.status,403);r.read();c.close()
  self.assertEqual(self.calls,0)
 def test_busy_service_has_bounded_threads_and_recovers(self):
  workers=[];errors=[]
  def wait_request():
   try:self.request(body=json.dumps(dict(message='wait',signature='fixture')))
   except Exception as e:errors.append(str(e))
  try:
   for _ in range(4):
    t=threading.Thread(target=wait_request);workers.append(t);t.start()
   deadline=time.monotonic()+2
   while self.waiting<4 and time.monotonic()<deadline:time.sleep(.01)
   self.assertEqual(self.waiting,4);status,_,headers=self.request();self.assertEqual(status,503);self.assertEqual(headers['Retry-After'],'2')
  finally:
   self.block.set()
   for t in workers:t.join(4)
  self.assertFalse(errors);self.assertEqual(self.request()[0],200)
 def test_rate_limit_is_retryable_without_authorization(self):
  self.server.tokens=0;self.server.rate=.001;self.assertEqual(self.request()[0],429);self.assertEqual(self.calls,0)

if __name__=='__main__':unittest.main()
