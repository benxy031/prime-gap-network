"""Bounded local index of records already admitted and verified by Core.

It consumes the incremental off-chain cursor. It creates no network blocks and
never reads a wallet or asks Core to sign or submit anything.
"""
import collections,contextlib,json,re,sqlite3,threading
from authority import Denied,Unavailable,SCRIPT,CHANNEL,ACCOUNT

class ContentIndex:
 def __init__(self,path,genesis,rpc):
  self.path,self.genesis,self.rpc=path,genesis,rpc;self.lock=threading.Lock();self.cache=collections.OrderedDict()
  with contextlib.closing(sqlite3.connect(path,timeout=1))as db:
   db.execute('PRAGMA journal_mode=WAL');db.execute('PRAGMA synchronous=FULL')
   db.execute('CREATE TABLE IF NOT EXISTS verified_content(id TEXT PRIMARY KEY,genesis TEXT NOT NULL,channel TEXT NOT NULL,owner TEXT NOT NULL,created_ms INTEGER NOT NULL)')
   db.execute('CREATE TABLE IF NOT EXISTS content_cursor(id INTEGER PRIMARY KEY CHECK(id=1),generation TEXT NOT NULL,cursor INTEGER NOT NULL)');db.commit()
 def tick(self):
  with contextlib.closing(sqlite3.connect(self.path,timeout=1))as db:
   row=db.execute('SELECT generation,cursor FROM content_cursor WHERE id=1').fetchone();generation,cursor=row if row else('',0)
   for _ in range(3):
    page=self.rpc('getsocialevents',[generation,cursor,50])
    if not isinstance(page,dict)or page.get('genesis')!=self.genesis:raise Unavailable('Core content belongs to another network')
    if page.get('loading'):return
    records=page.get('records');new_generation=page.get('generation');next_cursor=page.get('next_cursor')
    if not isinstance(records,list)or len(records)>50 or not isinstance(new_generation,str)or len(new_generation)>64 or type(next_cursor)is not int or next_cursor<0:raise Unavailable('Invalid bounded Core cursor')
    accepted=[]
    for record in records:
     message=record.get('canonical_message','')if isinstance(record,dict)else''
     if not isinstance(message,str)or len(message.encode())>16384 or not message.startswith('PGN/SOCIAL/EVENT/1\n'):continue
     try:event=json.loads(message.split('\n',1)[1])
     except ValueError:continue
     if event.get('genesis')!=self.genesis or event.get('kind')!='chat':continue
     target,channel,owner,created=(event.get(k)for k in ['event_id','channel','sender_script','created_ms'])
     if not isinstance(target,str)or not ACCOUNT.fullmatch(target)or not isinstance(channel,str)or not CHANNEL.fullmatch(channel)or not isinstance(owner,str)or not SCRIPT.fullmatch(owner)or type(created)is not int or created<0:continue
     accepted.append((target,self.genesis,channel,owner,created))
    db.execute('BEGIN IMMEDIATE')
    try:
     for item in accepted:
      old=db.execute('SELECT genesis,channel,owner,created_ms FROM verified_content WHERE id=?',(item[0],)).fetchone()
      if old is not None and old!=item[1:]:raise Unavailable('Core event identity changed')
      db.execute('INSERT OR IGNORE INTO verified_content VALUES(?,?,?,?,?)',item)
     db.execute('INSERT INTO content_cursor VALUES(1,?,?)ON CONFLICT(id)DO UPDATE SET generation=excluded.generation,cursor=excluded.cursor',(new_generation,next_cursor))
     db.execute('DELETE FROM verified_content WHERE id IN(SELECT id FROM verified_content ORDER BY created_ms DESC LIMIT -1 OFFSET 100000)');db.commit()
    except BaseException:db.rollback();raise
    if not page.get('more')or(next_cursor==cursor and new_generation==generation):break
    generation,cursor=new_generation,next_cursor
 def lookup(self,target):
  if not isinstance(target,str)or not ACCOUNT.fullmatch(target):return None
  with self.lock:
   if target in self.cache:self.cache.move_to_end(target);return dict(self.cache[target])
  with contextlib.closing(sqlite3.connect(self.path,timeout=.25))as db:row=db.execute('SELECT genesis,channel,owner FROM verified_content WHERE id=?',(target,)).fetchone()
  if row is None:return None
  value=dict(zip(['genesis','channel','owner'],row))
  with self.lock:
   self.cache[target]=value
   while len(self.cache)>2048:self.cache.popitem(last=False)
  return dict(value)

def projection(path,genesis,community,query):
 if set(query)!={'community','channel','targets'}or query['community']!=community:raise Denied('Wrong projection scope')
 channel=query['channel'];targets=query['targets'].split(',')if query['targets']else[]
 if not CHANNEL.fullmatch(channel)or len(targets)>50 or len(set(targets))!=len(targets)or any(not ACCOUNT.fullmatch(t)for t in targets):raise Denied('Bounded exact content targets required')
 with contextlib.closing(sqlite3.connect(path,timeout=.25))as db:
  revision=db.execute('SELECT revision FROM meta WHERE id=1').fetchone()[0]
  rows=db.execute('SELECT id,removed,pinned FROM content WHERE channel=? AND id IN('+','.join('?'for _ in targets)+')',[channel,*targets]).fetchall()if targets else[]
 return dict(schema='PGN_SOCIAL_PROJECTION_1',genesis=genesis,community=community,channel=channel,revision=revision,targets=targets,removed=[r[0]for r in rows if r[1]],pinned=[r[0]for r in rows if r[2]and not r[1]])
