"""CPU service computation for an existing own-Core attribution, before a search ticket.

This creates no allocation, credit, signature or scientific acceptance. The
anchor is checked while deriving the grant; its immutable identity excludes the
moving tip so exact retained computation survives an ordinary extension.
"""
import copy,hashlib,json
SCHEMA='PGN_CPU_NATIVE_AUDIT_SERVICE_WORK_V1'

class CpuServiceObservationChanged(RuntimeError):
    pass

def identity(value):
    return type(value)is str and len(value)==64 and value!='0'*64 and all(c in '0123456789abcdef'for c in value)

def service_request(worker,ticket,context):
    request=context.get('audit_request_id');slot=context.get('audit_slot');attempt=context.get('audit_attempt')
    if (context.get('role')!='audit' or context.get('work_cycle') or context.get('legacy_assignment')
            or not identity(request) or type(slot)is not int or slot not in(0,1)
            or type(attempt)is not int or not 0<=attempt<2**32):
        raise ValueError('Exact native audit service identity required')
    with worker.network_lock:
        address=worker.address()
        owned=worker.rpc.call('getaddressinfo',[address],wallet=True)
        actor=owned.get('scriptPubKey')
        if (owned.get('ismine')is not True or owned.get('iswatchonly')is True or type(actor)is not str
                or not actor or len(actor)>20000 or len(actor)%2 or any(c not in '0123456789abcdef'for c in actor)):
            raise ValueError('Native CPU audit actor is not owned by this wallet')
        status=worker.rpc.call('getpgnworkdutystatus',[address])
        view=worker.rpc.call('getpgnworkview',['audits',0,1,request]);state=view.get('state',{})
        anchor=state.get('bestblockhash');median=state.get('median_time_past');height=state.get('height')
        if (view.get('schema')!='PGN_WORK_VIEW_V1' or view.get('kind')!='audits'
                or state.get('genesis')!=worker.args.genesis or not identity(anchor)
                or type(height)is not int or height<0 or type(median)is not int or median<=0
                or status.get('schema')!='PGN_NATIVE_WORK_DUTY_STATUS_V1'
                or status.get('genesis')!=worker.args.genesis or not identity(status.get('bestblockhash'))
                or status.get('enforced')is not True or type(status.get('may_request'))is not bool):
            raise ValueError('CPU service requires a coherent own-Core native duty')
        reports=view.get('reports');audits=view.get('audits')
        if (not isinstance(reports,list) or len(reports)!=1 or not isinstance(audits,list) or len(audits)!=1
                or reports[0].get('request_id')!=request or audits[0].get('request_id')!=request):
            raise ValueError('Native CPU audit report identity changed')
        report=reports[0];jobs=[j for j in audits[0].get('jobs',[])if j.get('slot')==slot and j.get('attempt')==attempt]
        if len(jobs)!=1:raise ValueError('Native CPU audit slot/attempt changed')
        job=jobs[0]
        if (job.get('actor')!=actor or job.get('state')!='assigned' or job.get('expired')is not False
                or job.get('actor_policy_accepted')is not True or job.get('deadline_clock')!='median_chain_time'):
            raise ValueError('Native CPU audit is not the current owned permitted attribution')
        issued=job.get('issued');issued_time=job.get('issued_time');deadline=job.get('deadline_time')
        if (type(issued)is not int or not 0<issued<=height or type(issued_time)is not int
                or type(deadline)is not int or not 0<issued_time<=median<deadline):
            raise ValueError('Native CPU audit lease is not active on the verified chain')
        ranges=job.get('ranges');previous=-1;selected=None;origin=None
        if not isinstance(ranges,list)or len(ranges)!=5:raise ValueError('Five exact Core audit partitions required')
        for part in ranges:
            if type(part)is not dict or set(part)!={'index','start','end'}:raise ValueError('Exact Core audit partition fields required')
            index=part['index']
            if type(index)is not int or not previous<index<100:raise ValueError('Canonical Core audit indices required')
            previous=index
            for key in ('start','end'):
                value=part[key]
                if type(value)is not str or not value.isascii()or not value.isdecimal()or len(value)>40 or str(int(value))!=value:
                    raise ValueError('Canonical Core audit range required')
            if not(1<<67)<=int(part['start'])<int(part['end'])<=(1<<68):raise ValueError('Native CPU service supports Owned68 only')
            a,b=int(part['start']),int(part['end']);base=a-index*2000000000000
            if b-a!=2000000000000 or origin is not None and base!=origin:
                raise ValueError('Native CPU service partitions differ from the issued report range')
            origin=base
            part_id=hashlib.sha256(json.dumps(dict(domain='PGN_HORIZON_AUDIT_PARTITION_V1',request=request,slot=slot,attempt=attempt,index=index),sort_keys=True,separators=(',',':')).encode()).hexdigest()
            if part_id==ticket.get('ticket_id'):
                if ticket.get('start')!=part['start']or ticket.get('end')!=part['end']:raise ValueError('CPU service partition bounds differ from Core')
                selected=index
        if selected is None or not identity(report.get('ticket'))or not identity(report.get('report_root')):
            raise ValueError('CPU service partition/report differs from Core')
        if status['bestblockhash']!=anchor or worker.rpc.call('getbestblockhash')!=anchor:
            raise CpuServiceObservationChanged('Native CPU service anchor changed; retry before dequeue/computation')
        return dict(schema=SCHEMA,genesis=worker.args.genesis,actor=actor,request_id=request,
            report_ticket=report['ticket'],report_root=report['report_root'],slot=slot,attempt=attempt,
            issued=issued,issued_time=issued_time,deadline_time=deadline,deadline_clock='median_chain_time',
            ranges=copy.deepcopy(ranges),partition_index=selected,ticket_id=ticket['ticket_id'],start=ticket['start'],end=ticket['end'])
