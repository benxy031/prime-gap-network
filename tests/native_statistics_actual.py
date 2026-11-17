"""Real Core RPC calendar metadata, largest recorded gap and bounded history."""
from pathlib import Path
import argparse,contextlib,datetime as dt,json,struct,tempfile,time
from native_grouped249_actual import Node,file_sha
def bounds(name,now):
 day=now.replace(hour=0,minute=0,second=0,microsecond=0)
 if name=='today_utc':a,b=day,day+dt.timedelta(days=1)
 elif name=='hour_utc':a=now.replace(minute=0,second=0,microsecond=0);b=a+dt.timedelta(hours=1)
 elif name=='week_utc':a=day-dt.timedelta(days=day.weekday());b=a+dt.timedelta(days=7)
 else:a=day.replace(day=1);b=a.replace(year=a.year+1,month=1)if a.month==12 else a.replace(month=a.month+1)
 return int(a.timestamp()),int(b.timestamp())
def main():
 p=argparse.ArgumentParser();p.add_argument('--core',type=Path,required=True);p.add_argument('--history',type=Path,required=True);p.add_argument('--receipt',type=Path,required=True);a=p.parse_args();assert file_sha(a.history)=='09f318bfa826fad3d2af199ead9bb321dad039308864e549e3b92e6db6cfb7e4'
 result=dict(PASS=False,production_changed=False,screen_control=False);started=time.monotonic()
 try:
  with tempfile.TemporaryDirectory(prefix='pgn-statistics309-')as temporary:
   root=Path(temporary).resolve();assert root.parent==Path(tempfile.gettempdir()).resolve()and root.name.startswith('pgn-statistics309-')
   with contextlib.ExitStack()as cleanup:
    node=Node(a.core.resolve(),root/'node');cleanup.callback(node.close);node.rpc('getpgnleaderboard',[0,100]);times=[]
    with a.history.open('rb')as f:
     assert f.read(23)==b'PGN-PUBLIC-HISTORY-249\x00'
     for expected in range(322):
      height,size=struct.unpack('<II',f.read(8));identity=f.read(32).hex();raw=f.read(size);assert height==expected and 0<size<4000000 and len(raw)==size
      if height:assert node.rpc('submitblock',[raw.hex()])is None;times.append(node.rpc('getblockheader',[identity])['time'])
    total=node.rpc('getpgnleaderboard',[0,100]);assert total['rows']and set(total['supported_periods'])=={'total','today_utc','hour_utc','week_utc','month_utc'}
    exact=[]
    for row in total['rows']:
     value=row['largest_recorded_gap'];assert isinstance(value['complete'],bool)
     if 'gap'in value:
      assert value['complete']and isinstance(value['gap'],str)and value['gap']==str(int(value['gap']))and 0<int(value['gap'])<=2**32 and int(value['gap'])%2==0;exact.append(value['gap'])
    assert exact,'The real public fixture must expose at least one exact disclosed gap'
    periods={}
    for name in ('today_utc','hour_utc','week_utc','month_utc'):
     before=dt.datetime.now(dt.timezone.utc);page=node.rpc('getpgnleaderboard',[0,100,None,name]);after=dt.datetime.now(dt.timezone.utc)
     assert (page['utc_start'],page['utc_end'])in(bounds(name,before),bounds(name,after));assert page['period']==name and page['height']==321 and page['bestblockhash']==total['bestblockhash']and page['history_complete']
     anchored=node.rpc('getpgnleaderboard',[0,100,page['bestblockhash'],name,page['utc_start']]);assert anchored==page
     periods[name]=dict(start=page['utc_start'],end=page['utc_end'],rows=len(page['rows']))
     if name=='month_utc'and all(page['utc_start']<=t<page['utc_end']for t in times):assert page['rows']==total['rows'];result['month_total_parity']=True
    def rejected(args):
     try:node.rpc('getpgnleaderboard',args)
     except RuntimeError:return True
     return False
    now=dt.datetime.now(dt.timezone.utc);month_start=bounds('month_utc',now)[0]
    assert rejected([0,100,None,'year_utc'])and rejected([0,100,None,'month_utc',month_start+86400])and rejected([1,100,None,'week_utc'])
    old=(dt.datetime.fromtimestamp(month_start,dt.timezone.utc)-dt.timedelta(days=1)).replace(day=1);assert rejected([0,100,total['bestblockhash'],'month_utc',int(old.timestamp())]),'Unseen historical period was rebuilt'
    node.close();node.start();assert node.rpc('getpgnleaderboard',[0,100])==total;assert node.rpc('verifychain',[4,0])is True
    result.update(PASS=True,height=321,rows=len(total['rows']),exact_largest_gaps=exact,periods=periods,anchored_continuation=True,unseen_history_denied=True,cold_equals_warm=True,verifychain4=True,core_sha256=file_sha(a.core),seconds=round(time.monotonic()-started,3))
 except Exception as error:result['error']=str(error)[:1200];raise
 finally:a.receipt.parent.mkdir(parents=True,exist_ok=True);a.receipt.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
