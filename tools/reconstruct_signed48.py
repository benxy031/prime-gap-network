from pathlib import Path
import base64
import hashlib
import json
import struct
import sys
import zipfile

from raw_deflate import compress_raw

layout_path, prior, cohort, output = map(Path, sys.argv[1:])
layout = json.loads(layout_path.read_bytes())
assert layout['schema'] == 'PGN_QA_SIGNED_ZIP_LAYOUT_V1' and len(layout['entries']) == 425
assert layout['archive'] == '08787d16e6159e0687a1310db57a551fda1a13730b42439893f7a81264c9cfc6'
assert output.name == layout['archive']+'.components-48.zip'
assert layout['prior'] == 'a0f2554076c0c5ef86c1d3fecd0f1ec36e4dfedf7409c8a5f157cd8bc0331820'

def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()

assert digest(prior) == layout['prior']
assert digest(cohort/'components.json') == 'c60caac43396c0895a2ebe6c2a405b02e6dfbbaae72d9859bfd549cde6eb525a'
changes = {
    'payload/gpu/EngineRuntime/network/_internal/base_library.zip':'EngineRuntime/network/_internal/base_library.zip',
    'payload/gpu/EngineRuntime/network/horizon-engine-host.exe':'EngineRuntime/network/horizon-engine-host.exe',
    'payload/ui/PGN-Horizon.exe':'PGN-Horizon.exe',
    'payload/ui/README.txt':'README.txt',
    'payload/ui/assets/audit-animation.rcc':'assets/audit-animation.rcc',
}
assert {row['name'] for row in layout['entries'] if not row['reused']} == {'component-bundle.json', *changes}
partial = output.with_suffix('.partial')
if not output.exists():
    with zipfile.ZipFile(prior) as reference, prior.open('rb') as original_bytes, partial.open('xb') as target:
        assert set(reference.namelist()) == {item['name'] for item in layout['entries']}
        for item in layout['entries']:
            name, size = item['name'], item['size']
            target.write(base64.b64decode(item['prefix'], validate=True))
            expected = hashlib.sha256()
            if item['reused']:
                previous = reference.getinfo(name)
                assert previous.compress_size == size and not previous.flag_bits&8
                original_bytes.seek(previous.header_offset)
                header = original_bytes.read(30)
                name_size, extra_size = struct.unpack_from('<HH', header, 26)
                assert header[:4] == b'PK\x03\x04'
                original_bytes.seek(name_size+extra_size, 1)
                remaining = size
                while remaining:
                    block = original_bytes.read(min(remaining,1024*1024))
                    assert block
                    remaining -= len(block)
                    expected.update(block)
                    target.write(block)
            else:
                data = (base64.b64decode(layout['manifest'], validate=True) if name == 'component-bundle.json'
                        else (cohort/changes[name]).read_bytes())
                compressed = compress_raw(cohort/'zlib1.dll', data)
                assert len(compressed) == size
                expected.update(compressed)
                target.write(compressed)
            assert expected.hexdigest() == item['sha256'], name
        target.write(base64.b64decode(layout['tail'], validate=True))
    assert partial.stat().st_size==layout['bytes'] and digest(partial) == layout['archive']
    partial.rename(output)
assert digest(output) == layout['archive']
print(f"Verified reconstructed signed target of {layout['bytes']} bytes")
