"""Community authority follows the proved wallet key, independently of indexes.

Display labels and mining aliases never confer community permissions. The
configured moderator account is SHA256(exact signing script); another key needs
an explicit community grant. This keeps access usable while Native indexes load.
"""
import hashlib
from authority import Denied
from native_registry import NativeRegistry

class CommunityKeys(NativeRegistry):
 def verify_and_bind(self,script,address,message,signature):
  if self.rpc('getblockhash',[0])!=self.genesis:return False
  return super().verify_and_bind(script,address,message,signature)
 def resolve(self,script):
  if script not in self.addresses:raise Denied('A fresh wallet key proof is required')
  return hashlib.sha256(bytes.fromhex(script)).hexdigest()
