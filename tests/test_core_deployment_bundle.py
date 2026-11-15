from pathlib import Path
import copy,hashlib,json,sys,tempfile,unittest
P=Path(__file__).resolve().parents[1];sys.path.insert(0,str(P/'tools'))
import build_component_bundle as b

class CoreDescriptorTests(unittest.TestCase):
 def exercise(self,platform,mutation=None):
  with tempfile.TemporaryDirectory() as td:
   root=Path(td);(root/'Core').mkdir();name=Path(b.primary_programs(platform)['core']).name
   image=b'new qualified executable';(root/'Core'/name).write_bytes(image);digest=hashlib.sha256(image).hexdigest()
   descriptor=dict(schema='PGN_CORE_BUNDLE_V1',deployment=dict(product='PGN Core',genesis_hash='5'*64,executable=name,executable_sha256=digest),files=[dict(path=name,bytes=len(image),sha256=digest)])
   declared={'Core/'+name,'Core/core-template.json'}
   if mutation:mutation(descriptor,declared)
   (root/'Core/core-template.json').write_text(json.dumps(descriptor))
   b.validate_core_deployment(root,declared,dict(platform=platform,genesis_hash='5'*64))
 def test_qualified_windows_and_linux(self):
  for platform in ['windows-x86_64','linux-x86_64']:self.exercise(platform)
 def test_previous_executable_fingerprint_refused(self):
  for platform in ['windows-x86_64','linux-x86_64']:
   with self.assertRaisesRegex(ValueError,'startup fingerprint'):self.exercise(platform,lambda d,p:d['deployment'].update(executable_sha256='1'*64))
 def test_previous_file_manifest_refused(self):
  with self.assertRaisesRegex(ValueError,'descriptor fingerprint'):self.exercise('windows-x86_64',lambda d,p:d['files'][0].update(sha256='1'*64))
 def test_wrong_network_refused(self):
  with self.assertRaisesRegex(ValueError,'network differs'):self.exercise('linux-x86_64',lambda d,p:d['deployment'].update(genesis_hash='6'*64))
 def test_missing_template_refused(self):
  with self.assertRaisesRegex(ValueError,'requires its'):self.exercise('windows-x86_64',lambda d,p:p.remove('Core/core-template.json'))
 def test_missing_dependency_manifest_refused(self):
  with self.assertRaisesRegex(ValueError,'complete Core'):self.exercise('windows-x86_64',lambda d,p:p.add('Core/dependency.dll'))
 def test_wallet_settings_never_shipped(self):
  with self.assertRaisesRegex(ValueError,'user settings'):self.exercise('windows-x86_64',lambda d,p:d['deployment'].update(wallet='private-wallet'))

if __name__=='__main__':unittest.main()
