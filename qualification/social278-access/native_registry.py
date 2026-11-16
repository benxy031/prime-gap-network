"""Current-chain account binding, independent of display-name certificates."""
import hashlib
from authority import Denied,Unavailable,SCRIPT,ACCOUNT

class NativeRegistry:
 def __init__(self,rpc,genesis):self.rpc,self.genesis,self.addresses,self.anchors=rpc,genesis,{},{}
 def verify_and_bind(self,script,address,message,signature):
  if not isinstance(script,str)or not SCRIPT.fullmatch(script)or not isinstance(address,str)or not 8<=len(address)<=128:return False
  value=self.rpc('validateaddress',[address])
  if not isinstance(value,dict)or value.get('isvalid')is not True or value.get('scriptPubKey')!=script:return False
  if self.rpc('verifymessage',[address,signature,message])is not True:return False
  self.addresses[script]=address;return True
 def resolve(self,script):
  if script not in self.addresses:raise Denied('A fresh Native key proof is required before account lookup')
  root=self.rpc('getpgnparticipants',[0,1,self.addresses[script]])
  if not isinstance(root,dict)or root.get('genesis')!=self.genesis or type(root.get('height'))is not int or root['height']<0 or not isinstance(root.get('bestblockhash'),str)or not ACCOUNT.fullmatch(root['bestblockhash']):raise Unavailable('Native membership context unavailable')
  rows=root.get('participants')
  if not isinstance(rows,list)or len(rows)>1:raise Unavailable('Bounded exact Native account lookup required')
  # Exact NativeAuditOperator semantics: an admitted alias resolves to its
  # canonical account; otherwise the key's own script hash is its identity.
  # This authenticates an account, never a grant. Store policy separately
  # pins the operator-selected moderator IDs. No display certificate is used.
  if rows:
   row=rows[0]
   if not isinstance(row,dict)or row.get('signing_script')!=script or not isinstance(row.get('operator_id'),str)or not ACCOUNT.fullmatch(row['operator_id']):raise Unavailable('Native account lookup scope differs')
   account=row['operator_id'];source='Native admitted alias'
  else:account=hashlib.sha256(bytes.fromhex(script)).hexdigest();source='Native self-certifying script identity'
  if self.rpc('getblockhash',[root['height']])!=root['bestblockhash']:raise Unavailable('Native membership anchor changed; retry')
  self.anchors[script]=dict(height=root['height'],hash=root['bestblockhash'],binding=source);return account
