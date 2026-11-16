"""Community authority follows the proved wallet key, independently of indexes.

Display labels and mining aliases never confer community permissions. The
configured moderator account is SHA256(exact signing script); another key needs
an explicit community grant. This keeps access usable while Native indexes load.
"""
import hashlib
from authority import Denied
from native_registry import NativeRegistry

class CommunityKeys(NativeRegistry):
 def resolve(self,script):
  if script not in self.addresses:raise Denied('A fresh wallet key proof is required')
  return hashlib.sha256(bytes.fromhex(script)).hexdigest()
