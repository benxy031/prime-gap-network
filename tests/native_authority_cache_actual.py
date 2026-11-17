"""Actual isolated Core RPC: empty warm cache, first authority, cold equality."""
from pathlib import Path
import argparse,contextlib,hashlib,json,struct,tempfile,time
from native_grouped249_actual import Node,file_sha
def main():
 p=argparse.ArgumentParser();p.add_argument('--core',type=Path,required=True);p.add_argument('--history',type=Path,required=True);p.add_argument('--receipt',type=Path,required=True);a=p.parse_args()
 assert file_sha(a.history)=='09f318bfa826fad3d2af199ead9bb321dad039308864e549e3b92e6db6cfb7e4'
 result=dict(PASS=False,production_changed=False,screen_control=False);node=None;started=time.monotonic()
 try:
  with tempfile.TemporaryDirectory(prefix='pgn-authority310-')as temporary:
   root=Path(temporary).resolve();assert root.parent==Path(tempfile.gettempdir()).resolve()and root.name.startswith('pgn-authority310-')
   with contextlib.ExitStack()as cleanup:
    node=Node(a.core.resolve(),root/'node');cleanup.callback(node.close);first_empty=None;first_productive=None;early_errors=[];reads=0
    with a.history.open('rb')as f:
     assert f.read(23)==b'PGN-PUBLIC-HISTORY-249\x00'
     for expected in range(322):
      height,size=struct.unpack('<II',f.read(8));identity=f.read(32).hex();raw=f.read(size);assert height==expected and 0<size<4000000 and len(raw)==size
      if height:assert node.rpc('submitblock',[raw.hex()])is None
      assert node.rpc('getbestblockhash')==identity
      try:page=node.rpc('getpgnleaderboard',[0,100]);reads+=1
      except RuntimeError as error:
       # Before Native policies exist the read-only projection may be absent.
       # After the first empty snapshot ALL incremental refreshes must work.
       if first_empty is not None or expected>4:raise
       early_errors.append(dict(height=height,error=str(error)[:200]));continue
      assert page['schema']=='PGN_LEADERBOARD_V1'and page['height']==height and page['bestblockhash']==identity
      if not page['rows']and first_empty is None:first_empty=height
      if page['rows']and first_productive is None:first_productive=height
    assert first_empty is not None and first_productive is not None and first_productive>first_empty
    warm=node.rpc('getpgnleaderboard',[0,100]);assert warm['rows'];node.close();node.start();cold=node.rpc('getpgnleaderboard',[0,100]);assert cold==warm,'Incremental and cold leaderboard projections disagree'
    assert node.rpc('verifychain',[4,0])is True
    result.update(PASS=True,empty_cache_height=first_empty,first_productive_height=first_productive,incremental_reads=reads,rows=len(cold['rows']),height=cold['height'],cold_equals_warm=True,verifychain_level4=True,early_policy_errors=early_errors,core_sha256=file_sha(a.core),seconds=round(time.monotonic()-started,3))
 except Exception as error:result['error']=str(error)[:1200];raise
 finally:
  a.receipt.parent.mkdir(parents=True,exist_ok=True);a.receipt.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
