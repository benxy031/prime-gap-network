"""Test the production RCC command against timestamp and content changes."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile


def check(root, rcc, cmake, output):
    source = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    marker = 'SOURCE_DATE_EPOCH=1 QT_RCC_SOURCE_DATE_OVERRIDE=1'
    commands = [line.strip() for line in source.splitlines()
                if line.strip().startswith('COMMAND ') and marker in line]
    assert len(commands) == 1 and '--binary --no-compress' in commands[0]
    # Execute the exact production command via CMake, not a parallel imitation.
    command = commands[0].replace('${_pgn_audit_bundle}', '${CMAKE_CURRENT_BINARY_DIR}/fixture.rcc')
    command = command.replace('${CMAKE_CURRENT_SOURCE_DIR}/assets/audit-animation.qrc',
                              '${CMAKE_CURRENT_SOURCE_DIR}/fixture.qrc')
    def run(args, **kw):
        return subprocess.run([str(x) for x in args], check=True, capture_output=True,
                              timeout=60, **kw)
    with tempfile.TemporaryDirectory(prefix='pgn-rcc-repro-') as temp:
        fixture = Path(temp)
        payload = fixture / 'fixture.txt'
        payload.write_bytes(b'RESOURCE CONTENT A\n')
        qrc = fixture / 'fixture.qrc'
        qrc.write_text('<RCC><qresource prefix="/"><file alias="fixture.txt">fixture.txt</file></qresource></RCC>')
        project = '''cmake_minimum_required(VERSION 3.21)
project(ResourceIdentity NONE)
add_executable(Qt6::rcc IMPORTED)
set_property(TARGET Qt6::rcc PROPERTY IMPORTED_LOCATION "RCC_LOCATION")
add_custom_target(fixture ALL
  PRODUCTION_COMMAND
  VERBATIM)
'''.replace('RCC_LOCATION', rcc.resolve().as_posix()).replace('PRODUCTION_COMMAND', command)
        (fixture / 'CMakeLists.txt').write_text(project)
        build = fixture / 'build'
        run([cmake, '-S', fixture, '-B', build, '-G', 'Ninja'])
        os.utime(payload, (1700000000, 1700000000))
        run([cmake, '--build', build])
        first = (build / 'fixture.rcc').read_bytes()
        os.utime(payload, (1800000000, 1800000000))
        run([cmake, '--build', build])
        second = (build / 'fixture.rcc').read_bytes()
        assert first == second, 'Resource identity changed only because of source mtime'
        payload.write_bytes(b'RESOURCE CONTENT B\n')
        run([cmake, '--build', build])
        changed = (build / 'fixture.rcc').read_bytes()
        assert first != changed, 'Actual resource content change was hidden'
        # Negative control: plain rcc must show the original mtime dependency.
        clean = {k: v for k, v in os.environ.items()
                 if k not in ['SOURCE_DATE_EPOCH', 'QT_RCC_SOURCE_DATE_OVERRIDE']}
        payload.write_bytes(b'RESOURCE CONTENT A\n')
        old = fixture / 'unfixed.rcc'
        os.utime(payload, (1700000000, 1700000000))
        run([rcc, '--binary', '--no-compress', '-o', old, qrc], env=clean)
        old_first = old.read_bytes()
        os.utime(payload, (1800000000, 1800000000))
        run([rcc, '--binary', '--no-compress', '-o', old, qrc], env=clean)
        assert old_first != old.read_bytes(), 'Negative control did not expose the original defect'
        # Plain byte contents remain present in the intentionally uncompressed resource.
        assert b'RESOURCE CONTENT A\n' in first and b'RESOURCE CONTENT B\n' in changed
        receipt = dict(PASS=True, exact_production_command=True,
                       fixed_build_stable=True, actual_content_change_preserved=True,
                       negative_control_reproduced=True, bytes=len(first),
                       stable_sha256=hashlib.sha256(first).hexdigest(),
                       changed_sha256=hashlib.sha256(changed).hexdigest())
        if output:
            output.write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--rcc', type=Path, required=True)
    p.add_argument('--cmake', type=Path, required=True)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    check(args.root, args.rcc, args.cmake, args.output)
