import subprocess,sys
r=subprocess.run(sys.argv[1:],capture_output=True,timeout=25)
assert r.returncode!=0 and b'normal-miner controls were not forwarded'in r.stderr,(r.returncode,r.stdout[-800:],r.stderr[-800:])
print('PASS original CPU fallback incorrectly bypasses attached normal miner; exact regression reproduced')
