"""Build four exact CMake/Ninja object targets before the full native build."""
from pathlib import Path
import json,subprocess,os,ctypes,ctypes.wintypes
commands=json.loads(Path('build-native/compile_commands.json').read_text(encoding='utf8'))
wanted=['/src/pgn/social_identity_rpc.cpp','/src/pgn/social.cpp','/src/rpc/pgn_native.cpp','/src/init.cpp']
outputs=[]
for suffix in wanted:
 matches=[entry for entry in commands if entry['file'].replace('\\','/').endswith(suffix)]
 assert len(matches)==1,(suffix,len(matches))
 entry=matches[0]
 if 'output' in entry:output=entry['output']
 else:
  count=ctypes.c_int();dll=ctypes.WinDLL('shell32',use_last_error=True)
  dll.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.wintypes.LPWSTR);dll.CommandLineToArgvW.argtypes=[ctypes.wintypes.LPCWSTR,ctypes.POINTER(ctypes.c_int)]
  ptr=dll.CommandLineToArgvW(entry['command'],ctypes.byref(count));assert ptr
  argv=[ptr[i] for i in range(count.value)];ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.LocalFree(ptr)
  output=argv[argv.index('-o')+1]
 if Path(output).is_absolute():output=Path(output).relative_to(Path(entry['directory'])).as_posix()
 outputs.append(output.replace('\\','/'))
Path('consent-preflight-targets.json').write_text(json.dumps(dict(targets=outputs),indent=2)+'\n')
raise SystemExit(subprocess.call([os.environ['CMAKE_EXPLICIT'],'--build','build-native','--parallel','1','--target',*outputs]))
