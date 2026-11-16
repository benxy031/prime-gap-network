"""Private community access backend. No production key or transport is selected.

Every mutation is reevaluated against durable current state. The verifier is a
mandatory binding supplied by the Native-wallet transport, never a caller role.
"""
from dataclasses import dataclass
import hashlib,json,re,sqlite3,time,uuid

PERMISSIONS=frozenset({'read_messages','send_messages','react','manage_messages','manage_roles','manage_members','manage_channels'})
SCRIPT=re.compile(r'0014[0-9a-f]{40}\Z');ACCOUNT=re.compile(r'[0-9a-f]{64}\Z');ID=re.compile(r'[a-z0-9][a-z0-9_-]{0,63}\Z');CHANNEL=re.compile(r'[a-z0-9][a-z0-9_/-]{0,63}\Z')
DOMAIN='PGN/SOCIAL/ACCESS/1\n';_VERIFIED=object()
class Denied(Exception):pass
class Conflict(Exception):pass
class Unavailable(Exception):pass
def canonical(value):return json.dumps(value,ensure_ascii=False,sort_keys=True,separators=(',',':'))
def digest(value):return hashlib.sha256(value.encode()).hexdigest()
def text(value,maximum):
 if not isinstance(value,str)or not value.strip()or len(value)>maximum or any(ord(c)<32 for c in value):raise Denied('Invalid text')
 return value
@dataclass(frozen=True)
class Command:
 body:dict
 payload_hash:str
 _proof:object
class Verifier:
 def __init__(self,genesis,native_proof,clock=lambda:int(time.time()*1000),*,require_client=False):
  if not re.fullmatch('[0-9a-f]{64}',genesis)or not callable(native_proof):raise ValueError('Explicit Native proof/genesis binding required')
  self.genesis,self.native_proof,self.clock,self.require_client=genesis,native_proof,clock,require_client
 def verify(self,message,signature):
  try:return self._verify(message,signature)
  except(TypeError,ValueError,KeyError,OverflowError):raise Denied('Invalid access field types')from None
 def _verify(self,message,signature):
  if not isinstance(message,str)or len(message.encode())>16384 or not message.startswith(DOMAIN):raise Denied('Bounded access domain required')
  try:body=json.loads(message[len(DOMAIN):])
  except(ValueError,TypeError):raise Denied('Invalid access document')from None
  fields={'v','genesis','community','request_id','actor','address','created_ms','revision','action','args'}
  if not isinstance(body,dict)or set(body)not in(fields,fields|{'client'})or DOMAIN+canonical(body)!=message:raise Denied('Canonical exact access fields required')
  if self.require_client and'client'not in body:raise Denied('Explicit Social client binding required')
  if'client'in body and(not isinstance(body['client'],str)or not SCRIPT.fullmatch(body['client'])):raise Denied('Invalid Social client binding')
  if type(body['v'])is not int or body['v']!=1 or body['genesis']!=self.genesis or not SCRIPT.fullmatch(body['actor'])or not re.fullmatch('[0-9a-f]{64}',body['community']):raise Denied('Access identity mismatch')
  if str(uuid.UUID(body['request_id']))!=body['request_id']or type(body['created_ms'])is not int or type(body['revision'])is not int or body['revision']<0:raise Denied('Invalid operation identity/revision')
  if not isinstance(body['action'],str)or len(body['action'])>40 or not isinstance(body['args'],dict):raise Denied('Invalid access operation')
  text(body['address'],128)
  if not self.native_proof(body['actor'],body['address'],message,signature):raise Denied('Native proof refused')
  return Command(body,digest(message),_VERIFIED)

class Store:
 def __init__(self,path,genesis,moderators,*,create=False,content_lookup=None,clock=lambda:int(time.time()*1000),before_commit=None,account_registry=None):
  moderators=tuple(sorted(set(moderators)))
  self.account_registry=account_registry;self.member_pattern=ACCOUNT if account_registry is not None else SCRIPT
  if account_registry is not None and not callable(account_registry):raise ValueError('Verified current-chain account registry required')
  if not moderators or any(not self.member_pattern.fullmatch(s)for s in moderators):raise ValueError('Explicit signed account pins required')
  self.config=dict(v=2 if account_registry is not None else 1,genesis=genesis,moderators=moderators);self.community=digest(canonical(self.config));self.clock=clock
  self.content_lookup=content_lookup;self.before_commit=before_commit
  from pathlib import Path
  if not create and not Path(path).is_file():raise Unavailable('Access store absent; explicit bootstrap required')
  self.db=sqlite3.connect(path,timeout=5,isolation_level=None);self.db.row_factory=sqlite3.Row
  self.db.execute('PRAGMA journal_mode=WAL');self.db.execute('PRAGMA synchronous=FULL');self.db.execute('PRAGMA foreign_keys=ON')
  self.db.executescript('''CREATE TABLE IF NOT EXISTS meta(id INTEGER PRIMARY KEY CHECK(id=1),community TEXT NOT NULL,revision INTEGER NOT NULL);
   CREATE TABLE IF NOT EXISTS roles(id TEXT PRIMARY KEY,name TEXT NOT NULL,position INTEGER UNIQUE NOT NULL,permissions TEXT NOT NULL,managed INTEGER NOT NULL);
   CREATE TABLE IF NOT EXISTS members(id TEXT PRIMARY KEY,roles TEXT NOT NULL,banned INTEGER NOT NULL DEFAULT 0);
   CREATE TABLE IF NOT EXISTS overwrites(channel TEXT NOT NULL,target TEXT NOT NULL,member INTEGER NOT NULL,allow TEXT NOT NULL,deny TEXT NOT NULL,PRIMARY KEY(channel,target,member));
   CREATE TABLE IF NOT EXISTS receipts(actor TEXT NOT NULL,id TEXT NOT NULL,digest TEXT NOT NULL,result TEXT NOT NULL,created_ms INTEGER NOT NULL,PRIMARY KEY(actor,id));
   CREATE TABLE IF NOT EXISTS content(id TEXT PRIMARY KEY,channel TEXT NOT NULL,removed INTEGER NOT NULL DEFAULT 0,pinned INTEGER NOT NULL DEFAULT 0);
   CREATE TABLE IF NOT EXISTS reactions(target TEXT NOT NULL,actor TEXT NOT NULL,reaction TEXT NOT NULL,PRIMARY KEY(target,actor,reaction));
   CREATE INDEX IF NOT EXISTS receipt_age ON receipts(created_ms);''')
  row=self.db.execute('SELECT * FROM meta WHERE id=1').fetchone()
  if row is None:
   if not create:raise Unavailable('Access store incomplete; no automatic reset')
   self.db.execute('BEGIN IMMEDIATE')
   try:
    self.db.execute('INSERT INTO meta VALUES(1,?,0)',(self.community,))
    self.db.executemany('INSERT INTO roles VALUES(?,?,?,?,1)',[('everyone','Everyone',0,canonical(['read_messages','send_messages','react'])),('moderator','Moderator',999,canonical(sorted(PERMISSIONS)))])
    self.db.executemany('INSERT INTO members(id,roles)VALUES(?,?)',[(s,canonical(['moderator']))for s in moderators]);self.db.execute('COMMIT')
   except BaseException:self.db.execute('ROLLBACK');raise
  elif row['community']!=self.community:raise Unavailable('Authority changed; explicit migration required')
 def close(self):self.db.close()
 @property
 def revision(self):return self.db.execute('SELECT revision FROM meta WHERE id=1').fetchone()[0]
 def actor_account(self,script):
  if not SCRIPT.fullmatch(script):raise Denied('Native signing script required')
  if self.account_registry is None:return script # Private legacy key-only fixture.
  account=self.account_registry(script)
  if not isinstance(account,str)or not ACCOUNT.fullmatch(account):raise Denied('Native key has no current verified account')
  return account
 def member(self,actor):
  row=self.db.execute('SELECT * FROM members WHERE id=?',(actor,)).fetchone()
  ids=['everyone']+(json.loads(row['roles'])if row else[])
  roles=[self.db.execute('SELECT * FROM roles WHERE id=?',(r,)).fetchone()for r in ids]
  if any(r is None for r in roles):raise Unavailable('Member has an invalid role reference')
  return roles,bool(row and row['banned'])
 def permissions(self,actor,channel=''):
  roles,banned=self.member(actor)
  if banned:return set(),0
  result=set().union(*(set(json.loads(r['permissions']))for r in roles));rank=max(r['position']for r in roles)
  if channel:
   deny=set();allow=set()
   for r in roles:
    entry=self.db.execute('SELECT allow,deny FROM overwrites WHERE channel=?AND target=?AND member=0',(channel,r['id'])).fetchone()
    if entry:allow.update(json.loads(entry['allow']));deny.update(json.loads(entry['deny']))
   result=(result-deny)|allow
   entry=self.db.execute('SELECT allow,deny FROM overwrites WHERE channel=?AND target=?AND member=1',(channel,actor)).fetchone()
   if entry:result=(result-set(json.loads(entry['deny'])))|set(json.loads(entry['allow']))
  return result,rank
 def role(self,role):
  row=self.db.execute('SELECT * FROM roles WHERE id=?',(role,)).fetchone()
  if row is None:raise Denied('Unknown role')
  return row
 @staticmethod
 def permission_set(value):
  if not isinstance(value,list)or len(value)!=len(set(value))or not set(value)<=PERMISSIONS:raise Denied('Invalid permission set')
  return set(value)
 def apply(self,command):
  if not isinstance(command,Command)or command._proof is not _VERIFIED:raise Denied('Verified Native command required')
  b=command.body
  if digest(DOMAIN+canonical(b))!=command.payload_hash:raise Denied('Verified command bytes changed')
  if b['genesis']!=self.config['genesis']or b['community']!=self.community:raise Denied('Wrong authority scope')
  self.db.execute('BEGIN IMMEDIATE')
  try:
   actor=self.actor_account(b['actor'])
   previous=self.db.execute('SELECT digest,result FROM receipts WHERE actor=?AND id=?',(actor,b['request_id'])).fetchone()
   if previous:
    if previous['digest']!=command.payload_hash:raise Conflict('Operation ID reused for different bytes')
    result=json.loads(previous['result']);self.db.execute('ROLLBACK');return result
   if not self.clock()-120000<=b['created_ms']<=self.clock()+60000:raise Denied('Expired or future access command')
   if b['revision']!=self.revision:raise Conflict('Permission revision changed; refresh')
   permissions,rank=self.permissions(actor);action,args=b['action'],b['args']
   def require(permission):
    if permission not in permissions:raise Denied('Current service policy denies '+permission)
   def fields(*names):
    if set(args)!=set(names):raise Denied('Unexpected action arguments')
   def editable(role):
    entry=self.role(role)
    if entry['managed']or entry['position']>=rank:raise Denied('Role is managed or outside hierarchy')
    return entry
   if action in{'create_role','edit_role','delete_role','move_role'}:
    require('manage_roles')
    if action in{'create_role','edit_role'}:
     fields('role','name','position','permissions');r=args['role'];name=text(args['name'],80);p=args['position'];grants=self.permission_set(args['permissions'])
     if not ID.fullmatch(r)or type(p)is not int or not 0<p<rank or not grants<=permissions:raise Denied('Invalid hierarchy or permission grant')
     if action=='create_role':
      if self.db.execute('SELECT count(*)FROM roles').fetchone()[0]>=100:raise Denied('Role limit reached')
      self.db.execute('INSERT INTO roles VALUES(?,?,?,?,0)',(r,name,p,canonical(sorted(grants))))
     else:editable(r);self.db.execute('UPDATE roles SET name=?,position=?,permissions=?WHERE id=?',(name,p,canonical(sorted(grants)),r))
    elif action=='move_role':
     fields('role','position');editable(args['role']);p=args['position']
     if type(p)is not int or not 0<p<rank:raise Denied('Invalid role position')
     self.db.execute('UPDATE roles SET position=?WHERE id=?',(p,args['role']))
    else:
     fields('role');r=args['role'];editable(r)
     for member in self.db.execute('SELECT id,roles FROM members').fetchall():
      assigned=json.loads(member['roles'])
      if r in assigned:self.db.execute('UPDATE members SET roles=?WHERE id=?',(canonical([v for v in assigned if v!=r]),member['id']))
     self.db.execute('DELETE FROM overwrites WHERE target=?AND member=0',(r,));self.db.execute('DELETE FROM roles WHERE id=?',(r,))
   elif action in{'assign_role','remove_role','kick_member','ban_member'}:
    require('manage_members');fields('target','role')if action in{'assign_role','remove_role'}else fields('target');target=args['target']
    if not self.member_pattern.fullmatch(target)or self.permissions(target)[1]>=rank:raise Denied('Member is outside hierarchy')
    row=self.db.execute('SELECT roles,banned FROM members WHERE id=?',(target,)).fetchone();assigned=json.loads(row['roles'])if row else[];banned=bool(row and row['banned'])
    if action in{'assign_role','remove_role'}:
     role=args['role'];editable(role)
     if action=='assign_role':assigned=sorted(set(assigned)|{role})
     else:assigned=[r for r in assigned if r!=role]
     if len(assigned)>32:raise Denied('Member role limit reached')
    else:assigned=[];banned=action=='ban_member'
    self.db.execute('INSERT INTO members(id,roles,banned)VALUES(?,?,?)ON CONFLICT(id)DO UPDATE SET roles=excluded.roles,banned=excluded.banned',(target,canonical(assigned),int(banned)))
   elif action=='set_overwrite':
    require('manage_channels');fields('channel','target','member','allow','deny');channel=args['channel'];target=args['target'];member=args['member']
    allow=self.permission_set(args['allow']);deny=self.permission_set(args['deny'])
    if not CHANNEL.fullmatch(channel)or type(member)is not bool or allow&deny or not(allow|deny)<=permissions:raise Denied('Invalid channel overwrite')
    if member:
     if not self.member_pattern.fullmatch(target)or self.permissions(target)[1]>=rank:raise Denied('Member overwrite outside hierarchy')
    elif self.role(target)['position']>=rank:raise Denied('Role overwrite outside hierarchy')
    self.db.execute('INSERT INTO overwrites VALUES(?,?,?,?,?)ON CONFLICT(channel,target,member)DO UPDATE SET allow=excluded.allow,deny=excluded.deny',(channel,target,int(member),canonical(sorted(allow)),canonical(sorted(deny))))
   elif action in{'remove_content','pin_content','react_content'}:
    fields('target','channel','reaction','selected')if action=='react_content'else fields('target','channel');target=args['target'];channel=args['channel']
    if not re.fullmatch('[0-9a-f]{64}',target)or not CHANNEL.fullmatch(channel)or not self.content_lookup:raise Denied('Verified target unavailable')
    content=self.content_lookup(target)
    if not content or content['genesis']!=b['genesis']or content['channel']!=channel:raise Denied('Content target scope differs')
    effective,_=self.permissions(actor,channel)
    existing=self.db.execute('SELECT removed FROM content WHERE id=?',(target,)).fetchone()
    if existing and existing['removed']and action!='remove_content':raise Denied('Content is removed')
    if action=='remove_content':
     if content['owner']not in{b['actor'],actor}and'manage_messages'not in effective:raise Denied('No content removal permission')
    elif action=='pin_content':
     if'manage_messages'not in effective:raise Denied('No pin permission')
    else:
     if'react'not in effective or'read_messages'not in effective or not ID.fullmatch(args['reaction'])or type(args['selected'])is not bool:raise Denied('Reaction permission/value invalid')
     if args['selected']:self.db.execute('INSERT OR IGNORE INTO reactions VALUES(?,?,?)',(target,actor,args['reaction']))
     else:self.db.execute('DELETE FROM reactions WHERE target=?AND actor=?AND reaction=?',(target,actor,args['reaction']))
    if action!='react_content':
     self.db.execute('INSERT INTO content(id,channel,removed,pinned)VALUES(?,?,?,?)ON CONFLICT(id)DO UPDATE SET removed=max(removed,excluded.removed),pinned=CASE WHEN removed=1 OR excluded.removed=1 THEN 0 ELSE max(pinned,excluded.pinned)END',(target,channel,int(action=='remove_content'),int(action=='pin_content')))
     if action=='remove_content':self.db.execute('DELETE FROM reactions WHERE target=?',(target,))
   else:raise Denied('Unsupported access action')
   revision=self.revision+1;self.db.execute('UPDATE meta SET revision=?WHERE id=1',(revision,));result=dict(outcome='accepted',community=self.community,request_id=b['request_id'],revision=revision)
   self.db.execute('INSERT INTO receipts VALUES(?,?,?,?,?)',(actor,b['request_id'],command.payload_hash,canonical(result),b['created_ms']))
   self.db.execute('DELETE FROM receipts WHERE created_ms<?',(self.clock()-30*86400000,))
   if self.before_commit:self.before_commit()
   self.db.execute('COMMIT');return result
  except(sqlite3.IntegrityError,)as e:self.db.execute('ROLLBACK');raise Conflict('Conflicting role identity/position')from e
  except(TypeError,ValueError,KeyError,OverflowError)as e:
   if self.db.in_transaction:self.db.execute('ROLLBACK')
   raise Denied('Invalid action field types')from e
  except BaseException:
   if self.db.in_transaction:self.db.execute('ROLLBACK')
   raise
