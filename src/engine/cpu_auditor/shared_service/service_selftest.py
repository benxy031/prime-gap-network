"""Frozen CPU contract/checkpoint checks; no worker, wallet, Core or installation."""
from pathlib import Path
import copy,hashlib,json,os,sys,tempfile
import cycle_work
from native_transport import verify,HASHES

def require(value,message):
    if not value:raise RuntimeError(message)

def _run(Service,version):
    start=151219600000000000000;request='c'*64;slot=1;attempt=0;index=9
    identity=dict(domain='PGN_HORIZON_AUDIT_PARTITION_V1',request=request,slot=slot,attempt=attempt,index=index)
    ticket=hashlib.sha256(json.dumps(identity,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    ranges=[dict(index=i,start=str(start+i*2000000000000),end=str(start+(i+1)*2000000000000))for i in(0,9,20,40,99)]
    grant=dict(schema=cycle_work.SERVICE_SCHEMA,genesis='a'*64,actor='0014'+'d'*40,request_id=request,report_ticket='e'*64,report_root='f'*64,slot=slot,attempt=attempt,issued=233,issued_time=1000,deadline_time=87400,deadline_clock='median_chain_time',ranges=ranges,partition_index=index,ticket_id=ticket,start=ranges[1]['start'],end=ranges[1]['end'])
    checks={};require(len(cycle_work.partitions(grant))==100,'CPU service contract failed');checks['audit_before_ticket']=True
    for name,mutate in [('slot',lambda v:v.update(slot=0)),('attempt',lambda v:v.update(attempt=1)),('actor',lambda v:v.update(actor='invalid')),('lease',lambda v:v.update(deadline_time=v['issued_time'])),('fake_ticket',lambda v:v.update(normal_ticket='1'*64)),('expected_answer',lambda v:v.update(expected_counts=[0]*5)),('range',lambda v:v['ranges'][0].update(end=str(int(v['ranges'][0]['end'])-100)))]:
        value=copy.deepcopy(grant);mutate(value)
        try:cycle_work.partitions(value)
        except ValueError:checks['reject_'+name]=True
        else:raise RuntimeError('CPU accepted invalid service context: '+name)
    with tempfile.TemporaryDirectory(prefix='pgn-cpu-contract-')as temporary:
        cp=Path(temporary)/'checkpoint.json';service=Service.__new__(Service);service.cp=cp;service.request=None;service.enabled=False;service.new_work(dict(request=grant),False)
        restored=Service.__new__(Service);restored.cp=cp;restored.enabled=False;restored.restore()
        require(restored.request==grant and restored.counts==[0]*100 and restored.state=='PAUSED_RECOVERED'and not restored.enabled,'CPU service restart/OFF failed');checks['checkpoint_restart_and_off']=True
    images={};frozen=bool(getattr(sys,'frozen',False))
    if frozen:
        native=Path(sys._MEIPASS)/'native';suffix='.exe'if os.name=='nt'else''
        for name,expected in zip(('horizon-cpu-shared68-worker','horizon-cpu-inventory'),HASHES[os.name]):
            path=native/(name+suffix);verify(path,expected);images[name]=hashlib.sha256(path.read_bytes()).hexdigest()
        checks['qualified_native_images_unchanged']=True
    return dict(schema='PGN_CPU_CONTROLLER_SELF_TEST_V1',PASS=True,version=version,frozen=frozen,checks=checks,native_images=images,network_requested=False,installation_performed=False,native_worker_started=False,scientific_promotion=False)


def run(Service,version):
    from unittest.mock import patch
    def blocked(*args,**kwargs):raise RuntimeError('Self-test attempted network or child-process creation')
    with patch('socket.socket',blocked),patch('socket.create_connection',blocked),patch('subprocess.Popen',blocked):
        return _run(Service,version)
