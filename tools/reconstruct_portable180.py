from pathlib import Path
import base64
import hashlib
import json
import struct
import sys
import zipfile

layout, signed, output = map(Path, sys.argv[1:])
recipe = json.loads(layout.read_bytes())
expected = 'fcbd56743d495651072858454e72770e4a69202393343a3e9da3451874aa42f3'
assert recipe['schema']=='PGN_PORTABLE_ZIP_LAYOUT_V1' and len(recipe['rows'])==426
assert recipe['portable_sha256']==expected and recipe['portable_bytes']==267874254
assert recipe['signed_sha256']=='08787d16e6159e0687a1310db57a551fda1a13730b42439893f7a81264c9cfc6'

def digest(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle,'sha256').hexdigest()

assert digest(signed)==recipe['signed_sha256']
partial=output.with_suffix('.partial')
assert output.name=='PGN-Windows-180.zip' and not output.exists()
with zipfile.ZipFile(signed) as archive, signed.open('rb') as original, partial.open('xb') as target:
    for row in recipe['rows']:
        target.write(base64.b64decode(row['header'],validate=True))
        h=hashlib.sha256()
        if 'source' in row:
            item=archive.getinfo(row['source'])
            assert item.compress_size==row['bytes'] and not item.flag_bits&8
            original.seek(item.header_offset)
            header=original.read(30)
            assert header[:4]==b'PK\x03\x04'
            name,extra=struct.unpack_from('<HH',header,26)
            original.seek(name+extra,1)
            remaining=item.compress_size
            while remaining:
                data=original.read(min(remaining,1024*1024))
                assert data
                target.write(data)
                h.update(data)
                remaining-=len(data)
        else:
            data=base64.b64decode(row['inline'],validate=True)
            assert len(data)==row['bytes']
            target.write(data)
            h.update(data)
        assert h.hexdigest()==row['sha256']
    target.write(base64.b64decode(recipe['tail'],validate=True))
assert partial.stat().st_size==recipe['portable_bytes'] and digest(partial)==expected
partial.rename(output)
print(f'Portable archive SHA-256 verified: {expected}')
