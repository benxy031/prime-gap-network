"""Bounded loopback community API, intended behind an authenticated TLS edge.

No public listener or deployment is created by importing this module. Native
RPC credentials and policy storage remain exclusively on the service side.
"""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import json,threading,time
from authority import Denied,Conflict,Unavailable
MAX_BODY=20000;MAX_RESPONSE=1048576

class AccessServer(ThreadingHTTPServer):
 daemon_threads=True;request_queue_size=8;allow_reuse_address=False
 def __init__(self,address,backend_factory,*,requests_per_second=4,burst=8):
  if address[0]!='127.0.0.1':raise ValueError('The community backend must remain loopback-only behind TLS')
  if type(requests_per_second)not in{int,float}or not 0<requests_per_second<=100 or type(burst)is not int or not 1<=burst<=100:raise ValueError('Bounded service rate required')
  self.backend_factory=backend_factory;self.slots=threading.BoundedSemaphore(8);self.authorization_slots=threading.BoundedSemaphore(4);self.rate_lock=threading.Lock();self.rate=requests_per_second;self.burst=burst;self.tokens=float(burst);self.updated=time.monotonic()
  super().__init__(address,Handler)
 def admit(self):
  with self.rate_lock:
   now=time.monotonic();self.tokens=min(self.burst,self.tokens+(now-self.updated)*self.rate);self.updated=now
   if self.tokens<1:return False
   self.tokens-=1;return True
 def process_request(self,request,address):
  if not self.slots.acquire(False):
   # TCP capacity is exhausted. Do not pretend an HTTP reply was delivered
   # before a client has finished sending its request (Windows resets it).
   self.shutdown_request(request);return
  try:super().process_request(request,address)
  except BaseException:self.slots.release();raise
 def process_request_thread(self,request,address):
  try:super().process_request_thread(request,address)
  finally:self.slots.release()

class Handler(BaseHTTPRequestHandler):
 server_version='PGNCommunity';sys_version='';protocol_version='HTTP/1.1'
 def setup(self):super().setup();self.connection.settimeout(3)
 def log_message(self,*args):pass # No signatures, cookies, bodies or identities in access logs.
 def respond(self,status,value):
  raw=json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()
  if len(raw)>MAX_RESPONSE:status,raw=503,b'{"ok":false,"error":"Unavailable","reason":"Response budget exceeded"}'
  self.send_response(status);self.send_header('Content-Type','application/json; charset=utf-8');self.send_header('Content-Length',str(len(raw)));self.send_header('Cache-Control','no-store');self.send_header('Connection','close')
  if status in{429,503}:self.send_header('Retry-After','2')
  self.end_headers();self.close_connection=True;self.wfile.write(raw)
 def do_POST(self):
  if self.path!='/v1/access':self.respond(404,dict(ok=False,error='NotFound'));return
  if not self.server.admit():self.respond(429,dict(ok=False,error='Unavailable',reason='Community service busy; retry later'));return
  try:
   if self.headers.get('Transfer-Encoding')or len(self.headers.get_all('Content-Length',[]))!=1:raise Denied('One bounded content length required')
   length=self.headers['Content-Length']
   if not length.isascii()or not length.isdigit():raise Denied('Invalid content length')
   length=int(length)
   if length>MAX_BODY:self.respond(413,dict(ok=False,error='Denied',reason='Request exceeds budget'));return
   if length<2 or self.headers.get_content_type()!='application/json':raise Denied('JSON request required')
   raw=self.rfile.read(length)
   if len(raw)!=length:raise Denied('Incomplete request')
   value=json.loads(raw)
   if not isinstance(value,dict)or set(value)!={'message','signature'}or not isinstance(value['message'],str)or not isinstance(value['signature'],str)or not 1<=len(value['signature'])<=512:raise Denied('Invalid signed envelope')
   if not self.server.authorization_slots.acquire(False):self.respond(503,dict(ok=False,error='Unavailable',reason='Community authorization busy; retry later'));return
   try:
    backend=self.server.backend_factory()
    try:result=backend.handle(value['message'],value['signature'])
    finally:backend.store.close()
   finally:self.server.authorization_slots.release()
   self.respond(200,dict(ok=True,value=result))
  except Conflict as e:self.respond(409,dict(ok=False,error='Conflict',reason=str(e)))
  except(Denied,ValueError,UnicodeError)as e:self.respond(403,dict(ok=False,error='Denied',reason=str(e)))
  except(TimeoutError,OSError,Unavailable):self.respond(503,dict(ok=False,error='Unavailable',reason='Community service temporarily unavailable'))
  except Exception:self.respond(503,dict(ok=False,error='Unavailable',reason='Community authorization failed; no success reported'))
 def do_GET(self):self.respond(405,dict(ok=False,error='MethodNotAllowed'))
