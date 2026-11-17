"""Independent Gregorian datetime oracle for the compiled Native UTC bounds."""
import datetime as dt,hashlib,json,random,subprocess,sys
UTC=dt.timezone.utc;epoch=dt.datetime(1970,1,1,tzinfo=UTC)
def seconds(value):return int((value-epoch).total_seconds())
def expected(period,t):
 now=epoch+dt.timedelta(seconds=t);day=now.replace(hour=0,minute=0,second=0,microsecond=0)
 if period=='today_utc':a,b=day,day+dt.timedelta(days=1)
 elif period=='hour_utc':a=now.replace(minute=0,second=0,microsecond=0);b=a+dt.timedelta(hours=1)
 elif period=='week_utc':a=day-dt.timedelta(days=day.weekday());b=a+dt.timedelta(days=7)
 else:a=day.replace(day=1);b=a.replace(year=a.year+1,month=1)if a.month==12 else a.replace(month=a.month+1)
 return f'{seconds(a)} {seconds(b)}'
def verify(binary):
 randomizer=random.Random(239);rows=[];times=[0,1,3599,3600,86399,86400,4*86400-1,4*86400]
 for year in (1970,1999,2000,2024,2026,2027,2099,2100,2400,9998):
  for month in range(1,13):
   t=seconds(dt.datetime(year,month,1,tzinfo=UTC));times.extend([t,t+1,t+3599,t+86400-1])
   if t:times.append(t-1)
 times.extend(randomizer.randrange(0,253370678400)for _ in range(6000))
 for t in times:
  for period in ('today_utc','hour_utc','week_utc','month_utc'):
   answer=expected(period,t);rows.append((f'at {period} {t}',answer));rows.append((f'from {period} {answer.split()[0]}',answer))
 for period,value in [('today_utc',-1),('today_utc',3600),('hour_utc',1),('week_utc',0),('week_utc',-4*86400),('month_utc',86400),('month_utc',253402300800),('today_utc',2**63-1),('week_utc',2**63-1),('month_utc',-1),('bad',0)]:rows.append((f'from {period} {value}','ERR'))
 for period in ('today_utc','hour_utc','week_utc','month_utc'):rows.append((f'at {period} -1','ERR'))
 raw=('\n'.join(a for a,b in rows)+'\n').encode();r=subprocess.run([str(binary)],input=raw,capture_output=True,timeout=20);assert r.returncode==0,r.stderr[-1000:];actual=r.stdout.decode().splitlines();assert len(actual)==len(rows)
 for i,((query,answer),observed)in enumerate(zip(rows,actual)):assert answer==observed,(i,query,answer,observed)
 return dict(PASS=True,checks=len(rows),input_sha256=hashlib.sha256(raw).hexdigest())
if __name__=='__main__':print(json.dumps(verify(sys.argv[1])))
