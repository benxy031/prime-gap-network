"""Private Native-proof adapter. No network listener, public account or grants.

Every read and mutation carries a fresh, domain-scoped wallet proof. UI session
references are public observation IDs, never bearer credentials or authority.
"""
import json,time
from authority import Denied,Conflict,Unavailable,PERMISSIONS,digest,canonical

class Backend:
 def __init__(self,store,verifier,labels=None):self.store,self.verifier,self.labels=store,verifier,labels or{}
 def handle(self,message,signature):
  command=self.verifier.verify(message,signature);b=command.body
  if b['community']!=self.store.community:raise Denied('Wrong community')
  if b['action']=='read_access':
   if not self.store.clock()-120000<=b['created_ms']<=self.store.clock()+60000:raise Denied('Expired read proof')
   if set(b['args'])!={'channel','member','role'}:raise Denied('Invalid read scope')
   return self.snapshot(b['actor'],command.payload_hash,b['args'],b.get('client',b['actor']))
  result=self.store.apply(command)
  return dict(result,identity=b.get('client',b['actor']))
 def snapshot(self,actor,proof,focus,client):
  from authority import CHANNEL,SCRIPT,ID
  channel,member,selected=focus['channel'],focus['member'],focus['role']
  if not isinstance(channel,str)or not CHANNEL.fullmatch(channel):raise Denied('Invalid channel')
  if not isinstance(member,str)or(member and not self.store.member_pattern.fullmatch(member)):raise Denied('Invalid focused member')
  if not isinstance(selected,str)or(selected and not ID.fullmatch(selected)):raise Denied('Invalid focused role')
  db=self.store.db;db.execute('BEGIN')
  try:
   account=self.store.actor_account(actor);revision=self.store.revision;perms,rank=self.store.permissions(account)
   roles=[dict(id=r['id'],name=r['name'],position=r['position'],permissions=json.loads(r['permissions']),managed=bool(r['managed']))for r in db.execute('SELECT * FROM roles ORDER BY position DESC LIMIT 101')]
   if len(roles)>100:raise Unavailable('Role page limit exceeded')
   members=[dict(id=m['id'],name=self.labels.get(m['id'],'Miner '+m['id'][:8]+'…'+m['id'][-6:]),roles=['everyone']+json.loads(m['roles']),banned=bool(m['banned']))for m in db.execute('SELECT * FROM members ORDER BY id LIMIT 201')]
   if len(members)>200:raise Unavailable('Focused member pagination required')
   if not member:member=members[0]['id']if members else account
   overwrites=[dict(channel=r['channel'],target=r['target'],member=bool(r['member']),allow=json.loads(r['allow']),deny=json.loads(r['deny']))for r in db.execute('SELECT * FROM overwrites WHERE channel=? LIMIT 501',(channel,))]
   if len(overwrites)>500:raise Unavailable('Channel page limit exceeded')
   availability=[]
   def offer(action,allowed,**scope):
    row=dict(action=action,allowed=bool(allowed),reason=''if allowed else'Current service policy denies this action',target='',role='',channel='',scope='social',parent='',reaction='');row.update(scope);availability.append(row)
   # Server-side presentation only; apply() repeats full authorization on every
   # mutation. Focus bounds avoid the roles x members Cartesian product.
   offer('create_role','manage_roles'in perms and rank>1)
   target_rank=self.store.permissions(member)[1]
   for r in roles:
    editable=not r['managed']and r['position']<rank
    for action in ['edit_role','delete_role','move_role']:offer(action,editable and'manage_roles'in perms,role=r['id'])
    for action in ['assign_role','remove_role']:offer(action,editable and'manage_members'in perms and target_rank<rank,target=member,role=r['id'])
    offer('set_overwrite','manage_channels'in perms and r['position']<rank,target=r['id'],role=r['id'],channel=channel)
   for action in ['kick_member','ban_member']:offer(action,'manage_members'in perms and target_rank<rank,target=member)
   for m in members:offer('set_overwrite','manage_channels'in perms and self.store.permissions(m['id'])[1]<rank,target=m['id'],channel=channel)
   if len(availability)>1000:raise Unavailable('Focused action page limit exceeded')
   now=self.store.clock();session=dict(identity=client,state='verified',reference=proof,verification_ref='native:'+proof,revocation_revision=str(revision),device=actor,expires_ms=now+120000,devices=[dict(id=actor,label='Verified Native wallet',revoked=False)],flows=[])
   result=dict(identity=client,account=account,genesis=self.store.config['genesis'],authenticated=True,community=self.store.community,revision=str(revision),source='Native verified community access',observed_ms=now,moderator_role_id='moderator',roles=roles,members=members,overwrites=overwrites,permissions=sorted(PERMISSIONS),grantable=sorted(perms),availability=availability,session=session)
   db.execute('ROLLBACK');return result
  except BaseException:
   if db.in_transaction:db.execute('ROLLBACK')
   raise
