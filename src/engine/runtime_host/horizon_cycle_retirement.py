"""Conserve inactive native-service cycles before releasing hot scheduling slots."""
import copy,json
from contextlib import ExitStack
from report_commitment import canonical_bytes
from pgn_session import atomic_bytes
from horizon_work_cycle import WorkCycleJournal,identity
from horizon_ticket_archive import SCHEMA as COMPLETED_SCHEMA,ARCHIVE_NAME


def observe_inactive(rpc,cycle,genesis):
    """Absence is usable only in an exact, coherent owned-Core work view.

    This observation is local scheduling evidence, never a network verdict.
    A submitted report or an extant lease keeps its current recovery path.
    """
    work=cycle['work'];request=work['request_id']
    view=rpc.call('getpgnworkview',['tickets',0,1,request])
    state=view.get('state',{})
    if (view.get('schema')!='PGN_WORK_VIEW_V1' or view.get('kind')!='tickets'
        or view.get('request_id')!=request or state.get('genesis')!=genesis
        or type(view.get('offset'))is not int or view['offset']!=0
        or type(view.get('limit'))is not int or view['limit']!=1
        or type(state.get('lease_offset'))is not int or state['lease_offset']!=0
        or not identity(state.get('bestblockhash')) or type(state.get('height'))is not int or state['height']<0
        or type(state.get('leases'))is not list or type(view.get('reports'))is not list):
        raise ValueError('Exact owned-Core work view required')
    if len(state['leases'])>1 or any(v.get('request')!=request for v in state['leases']):
        raise ValueError('Work view returned another reservation')
    if state['leases'] or view['reports']:return None
    anchor=state['bestblockhash']
    if rpc.call('getbestblockhash')!=anchor:return None
    return dict(schema='PGN_INACTIVE_LOCAL_CYCLE_V1',genesis=genesis,request_id=request,
        previous_ticket=work['ticket'],anchor=anchor,height=state['height'],reason='no_current_lease_or_report')


def retain_inactive(journal,request,evidence,*,protected_requests,expected_cycle,physical_service=None):
    """Commit an immutable conservation record before releasing one hot slot.

    Caller owns/stops the executors involved and supplies their current protected
    requests. No result, source frontier, report, wallet or controller is touched.
    Native receipts remain immutable and their accounting stays with Core.
    """
    with journal.lock:
        if journal.owner.fd is None:raise RuntimeError('Work-cycle writer is closed')
        if request in protected_requests:raise RuntimeError('Cycle still belongs to an executor or preparation lane')
        current=next((c for c in journal.value['cycles']if c['work']['request_id']==request),None)
        if current is None:return False
        if current!=expected_cycle:raise ValueError('Cycle changed after inactivity observation')
        if (current['stage'] not in('prepared','running') or current['hold']
            or 'native_service'not in current or not WorkCycleJournal.ready(current)):
            raise ValueError('Only an unheld native-service cycle can be retired by this retirement path')
        required={'schema','genesis','request_id','previous_ticket','anchor','height','reason'}
        if (type(evidence)is not dict or set(evidence)!=required
            or evidence['schema']!='PGN_INACTIVE_LOCAL_CYCLE_V1' or evidence['genesis']!=journal.value['genesis']
            or evidence['request_id']!=request or evidence['previous_ticket']!=current['work']['ticket']
            or not identity(evidence['anchor']) or type(evidence['height'])is not int or evidence['height']<0
            or evidence['reason']!='no_current_lease_or_report'):
            raise ValueError('Inactivity evidence differs from its cycle')
        payload=dict(cycle=current,evidence=evidence,extra_audits=journal.extra_audits(request))
        if physical_service is not None:payload['physical_service']=copy.deepcopy(physical_service)
        raw=canonical_bytes(payload)
        with journal.history.database(create=True)as db:
            db.execute('CREATE TABLE IF NOT EXISTS inactive_native_cycles(request TEXT NOT NULL,ticket TEXT NOT NULL,receipt BLOB NOT NULL CHECK(length(receipt)<=65536),PRIMARY KEY(request,ticket)) WITHOUT ROWID')
            prior=db.execute('SELECT receipt FROM inactive_native_cycles WHERE request=? AND ticket=?',(request,current['work']['ticket'])).fetchone()
            if prior is not None:
                retained=json.loads(prior[0])
                if (retained['cycle']!=current or retained['extra_audits']!=journal.extra_audits(request)
                    or retained.get('physical_service')!=physical_service):
                    raise ValueError('Immutable inactive cycle changed')
            db.execute('INSERT OR IGNORE INTO inactive_native_cycles VALUES(?,?,?)',(request,current['work']['ticket'],raw))
        # A crash here leaves both copies. Restart can repeat the same observation
        # and write, while the existing hot journal remains authoritative.
        candidate=copy.deepcopy(journal.value)
        candidate['cycles']=[c for c in candidate['cycles']if c['work']['request_id']!=request]
        journal.validate(candidate)
        atomic_bytes(journal.file,canonical_bytes(candidate)+b'\n')
        journal.value=candidate
        return True


def _request(requests,value):
    if value is None:return
    if not identity(value):raise ValueError('Malformed protected work request')
    requests.add(value)


def _work(requests,value):
    if value is None:return
    if type(value)is not dict:raise ValueError('Malformed protected work')
    _request(requests,value.get('request_id'))


def _context(requests,value):
    if type(value)is not dict:raise ValueError('Malformed saved GPU context')
    for key in ('work_cycle','audit_budget_cycle','audit_request_id'):_request(requests,value.get(key))
    _work(requests,value.get('work'))


def _saved(path,limit=32768):
    if not path.exists():return None
    if path.stat().st_size>limit:raise ValueError('Protected work journal exceeds budget')
    with path.open('rb')as stream:raw=stream.read(limit+1)
    if len(raw)>limit:raise ValueError('Protected work journal grew beyond budget')
    value=json.loads(raw)
    if type(value)is not dict:raise ValueError('Malformed protected work journal')
    return value


def _reassigned_requests(flow):
    queue=getattr(flow,'reassignments',None);requests=set()
    if queue is not None:
        # This lock is never acquired under the GPU condition. Existing
        # reassignment adoption can acquire its own lock from a scheduler handoff.
        with queue.lock:
            for key in queue.read():
                value=queue.record(key)
                _work(requests,value.get('previous'));_work(requests,value.get('work'))
    return requests


def _physical_service(flow,request):
    physical=getattr(flow,'physical_audits',None)
    if physical is None:return None
    with physical.lock:
        if not any(request in value for value in (physical.cycles,physical.native,physical.extras)):return None
        return copy.deepcopy(dict(tasks=physical.cycles.get(request),native=physical.native.get(request),
                                  completed_extras=physical.extras.get(request)))


def _protected(flow,incoming,reassigned):
    """Caller holds the scheduler and report-queue locks; no RPC is performed."""
    requests={incoming['request_id'],*reassigned}
    gpu=flow.gpu
    jobs=list(gpu.jobs.values())
    jobs.extend(job for job in (getattr(gpu,'active',None),getattr(gpu,'resume_job',None))if job is not None)
    for job in jobs:_context(requests,job.context)
    if getattr(flow.worker,'cpu_only',False)is True:return requests
    _work(requests,getattr(flow.worker,'current',None))
    recovered=getattr(flow,'recovered_report',None)
    if recovered is not None:_work(requests,recovered[0])
    prefetch=getattr(flow,'prefetch',None)
    if prefetch is not None:
        with prefetch.condition:_work(requests,prefetch.ready)
    root=flow.worker.root
    for path in (root/'controller.json',root/'prepared-ticket'/'controller.json'):
        saved=_saved(path)
        if saved is not None and saved.get('stage')!='CONSUMED':_work(requests,saved.get('work'))
    saved=_saved(root/'gpu-active.json')
    if saved is not None:_context(requests,saved.get('context',{}))
    for lane in getattr(flow,'audit_lanes',()):
        audit=getattr(lane,'current_audit',None)
        for value in (audit,_saved(lane.folder/'current.json',262144)):
            if value is not None:
                for key in ('physical_cycle','work_cycle','request_id'):_request(requests,value.get(key))
    outbox=getattr(flow,'outbox',None)
    if outbox is not None:
        for entry in outbox._read():_work(requests,entry['work'])
    preseal=getattr(flow,'preseal',None)
    if preseal is not None:
        for entry in preseal.entries():_request(requests,entry['request'])
    # Conserve a report written before its queue index (or after an interrupted
    # recovery). These are exact ticket directories, not an archive/history scan.
    for cycle in flow.cycles.value['cycles']:
        work=cycle['work'];request=work['request_id'];folder=root/'tickets'/work['ticket']
        physical=_physical_service(flow,request)
        if physical is not None and any(task and task['stage']=='assigned'for task in (physical['tasks']or [])):
            requests.add(request)
        completed=physical['completed_extras']if physical is not None else {}
        if set(flow.cycles.extra_audits(request))-set(completed or {}):
            requests.add(request)
        if any((folder/name).exists()for name in ('report.json','network-report.json','preseal-input.json','preseal-final.json',ARCHIVE_NAME)):
            requests.add(work['request_id'])
        session=_saved(folder/'session.json',262144)
        if session is not None and (session.get('state')=='COMPLETE' or session.get('schema')==COMPLETED_SCHEMA):
            requests.add(work['request_id'])
    return requests


def _queue_locks(stack,flow):
    if getattr(flow.worker,'cpu_only',False)is True:return
    # Scheduler already takes condition -> outbox/preseal for capacity checks.
    # Hold both through retention so a preseal->outbox transfer cannot disappear
    # between two independently sampled queues. Neither queue performs RPC here.
    for name in ('outbox','preseal'):
        queue=getattr(flow,name,None)
        if queue is not None:stack.enter_context(queue.lock)


def _inactive_allocation(journal,work):
    """Conservation is not scheduling recovery. Never silently recreate its state."""
    with journal.history.database()as db:
        if db is None or not db.execute("SELECT 1 FROM sqlite_master WHERE type='table'AND name='inactive_native_cycles'").fetchone():return False
        row=db.execute('SELECT CASE WHEN length(receipt)<=65536 THEN receipt END FROM inactive_native_cycles WHERE request=? AND ticket=?',
                       (work['request_id'],work['ticket'])).fetchone()
    if row is None:return False
    if row[0]is None:raise ValueError('Retained inactive allocation exceeds its budget')
    value=json.loads(row[0])
    if (type(value)is not dict or value.get('cycle',{}).get('work',{}).get('request_id')!=work['request_id']
        or value['cycle']['work'].get('ticket')!=work['ticket']):
        raise ValueError('Retained inactive allocation differs from its exact index')
    journal.validate(dict(schema='PGN_WORK_CYCLES_V1',genesis=journal.value['genesis'],cycles=[value['cycle']]))
    return True


def reconcile_capacity(flow,incoming):
    """Free one proved inactive hot slot, preserving its exact unfinished state."""
    if flow.cycles.contains(incoming['request_id']):return False
    if _inactive_allocation(flow.cycles,incoming):
        raise RuntimeError('Retained inactive allocation reappeared; recover its original local cycle before readmission')
    with flow.cycles.lock:
        if len(flow.cycles.value['cycles'])<flow.cycles.maximum_cycles:return False
        if any(c['stage']=='completed'and not c['hold']for c in flow.cycles.value['cycles']):return False
        candidates=copy.deepcopy(flow.cycles.value['cycles'])
    for cycle in candidates:
        request=cycle['work']['request_id']
        if cycle['stage']not in('prepared','running')or cycle['hold']or 'native_service'not in cycle or not WorkCycleJournal.ready(cycle):continue
        with flow.worker.network_lock:
            # Local network/preparation writers use this lane. Reassignment
            # snapshots precede the GPU lock; removals only over-protect work.
            reassigned=_reassigned_requests(flow)if getattr(flow.worker,'cpu_only',False)is not True else set()
            with flow.gpu.condition:
                with ExitStack()as stack:
                    _queue_locks(stack,flow)
                    if request in _protected(flow,incoming,reassigned):continue
            proof=observe_inactive(flow.worker.rpc,cycle,flow.worker.args.genesis)
            if proof is None:continue
            reassigned|=_reassigned_requests(flow)if getattr(flow.worker,'cpu_only',False)is not True else set()
            with flow.gpu.condition:
                with ExitStack()as stack:
                    _queue_locks(stack,flow)
                    protected=_protected(flow,incoming,reassigned)
                    if request in protected:continue
                    return retain_inactive(flow.cycles,request,proof,protected_requests=protected,expected_cycle=cycle,
                                           physical_service=_physical_service(flow,request))
    return False


def reconcile_cpu_capacity(flow,incoming):
    # Preserve the existing CPU-only entry point for callers and regressions.
    if getattr(flow.worker,'cpu_only',False)is not True:return False
    return reconcile_capacity(flow,incoming)
