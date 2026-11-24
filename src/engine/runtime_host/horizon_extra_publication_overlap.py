"""Same-ticket GPU computation while one exact sealed extra awaits publication.

No native credit, waiver, report or official cycle promotion is produced here.
Completed source is retained under the existing preseal-required archive barrier.
"""
from concurrent.futures import CancelledError
from pathlib import Path
from copy import deepcopy
import hashlib,json,time
from pgn_session import atomic_bytes
from report_commitment import canonical_bytes,validate_computed_report
from horizon_compute_lease import computation_lease_active
from horizon_native_work_duty import context as native_context,identity
from horizon_service_presentation import audit_view_ref
from horizon_core_rpc import CoreRpcError
from horizon_audit_batch import ready_audit_view,_ready_decline


def candidate(flow,job):
    if (getattr(flow.worker,'cpu_only',False) or flow.finalizer is None
            or job.context.get('role')!='search' or not job.context.get('prepared')
            or getattr(job,'completed_result',None) is not None):return None
    cycle=job.context.get('work_cycle');state=flow.cycles.snapshot(cycle);physical=flow.physical_audits
    if (state['stage']!='prepared' or state['hold'] or 'native_service'not in state
            or state['work']!=flow.cycles.work_identity(job.context['work'])
            or not flow.cycles.ready(state) or not physical.is_native(cycle)):
        return None
    tasks=physical.snapshot(cycle)
    if not tasks or any(not task or task['stage']not in('committed','unavailable')for task in tasks):return None
    extras=flow.cycles.extra_audits(cycle)
    if len(extras)!=1 or physical.catchup_ready(cycle):return None
    for lane in flow.audit_lanes:
        audit=lane.current_audit
        if (not audit or not audit.get('answer')or audit.get('commit_ready')is not True
                or len(audit.get('ranges',[]))!=5 or len(audit.get('counts',[]))!=5):continue
        ref=audit_view_ref(audit.get('request_id'),audit.get('slot'),audit.get('attempt'))
        if (ref is not None and ref['audit_id']==extras[0]
                and flow.cycles.extra_audit_cycle(extras[0])==cycle):
            return lane,audit,dict(audit_id=extras[0],commitment=audit['answer']['commitment'])
    return None


def ticket_status(flow,work):
    """One coherent owned-Core allocation view; never issue/sign/request work."""
    rpc=flow.worker.rpc;fresh=None
    try:fresh=rpc.call('getpgnscientificwork',[work['request_id'],'0'*64])
    except CoreRpcError as error:
        if error.code!=-8:raise
    view=rpc.call('getpgnworkview',['tickets',0,1,work['request_id']]);state=view.get('state',{})
    if (view.get('schema')!='PGN_WORK_VIEW_V1'or view.get('kind')!='tickets'
        or view.get('request_id')!=work['request_id']or view.get('offset')!=0 or view.get('limit')!=1
        or state.get('genesis')!=work['genesis']or state.get('lease_offset')!=0
        or not identity(state.get('bestblockhash'))
        or type(state.get('height'))is not int or type(state.get('leases'))is not list
        or type(view.get('reports'))is not list):raise ValueError('Exact ticket projection required')
    anchor=state.get('bestblockhash')
    if rpc.call('getbestblockhash')!=anchor:return dict(status='retry')
    if view['reports']:return dict(status='retry')
    leases=state['leases']
    if not leases:return dict(status='absent',anchor=anchor)
    if len(leases)!=1 or leases[0].get('request')!=work['request_id']:
        raise ValueError('Exact ticket lease required')
    lease=leases[0]
    if not computation_lease_active(state,lease):return dict(status='expiry_pending',anchor=anchor)
    if (not fresh or fresh.get('schema')!='PGN_NATIVE_SCIENTIFIC_WORK1'or fresh.get('accepted')is not False
        or fresh.get('genesis')!=work['genesis']or fresh.get('request_id')!=work['request_id']
        or fresh.get('parent')!=anchor or fresh.get('height')!=state['height']+1):return dict(status='retry')
    for key in ('miner_script','reward_script'):
        if fresh.get(key)!=work.get(key)or lease.get(key)!=work.get(key):raise ValueError('Ticket owner changed')
    if fresh.get('ticket')!=work['ticket']:return dict(status='reassigned',anchor=anchor)
    if (any(fresh.get(k)!=work[k]for k in ('start','length'))or lease.get('start')!=work['start']
        or lease.get('end')!=str(int(work['start'])+int(work['length']))):raise ValueError('Ticket interval changed')
    service=fresh.get('native_audit_service',{})
    if service.get('view_parent')!=anchor:raise ValueError('Ticket service anchor changed')
    normalized=native_context({k:v for k,v in service.items()if k!='view_parent'},work)
    if normalized!=flow.cycles.snapshot(work['request_id']).get('native_service'):
        return dict(status='retry')
    return dict(status='same',anchor=anchor)


def _regular_gate(flow,cycle,anchor=None):
    """Recheck existing physical confirmations/waivers at a stable Core tip."""
    physical=flow.physical_audits;tasks=physical.snapshot(cycle)
    if not tasks or any(not task or task['stage']not in('committed','unavailable')for task in tasks):return False
    observed=flow.worker.rpc.call('getbestblockhash')
    if anchor is not None and observed!=anchor:return False
    anchor=observed
    for task in tasks:
        if task['stage']=='unavailable'and task['anchor']==anchor:continue
        if not flow.audit_reference_current(task):return False
    return (flow.worker.rpc.call('getbestblockhash')==anchor and physical.snapshot(cycle)==tasks
            and not flow.cycles.snapshot(cycle)['hold'])


def verify_start(flow,job):
    ready=candidate(flow,job)
    if ready is None:return False
    lane,audit,ref=ready;name=f"{audit['request_id']}-{audit['slot']}-{audit['attempt']}"
    sealed=lane.answers.recover_seal(name,audit,require_durable=True)
    if sealed is None or sealed.get('answer')!=audit['answer']:return False
    status=ticket_status(flow,job.context['work'])
    if status['status']!='same':return False
    anchor=status['anchor']
    if not _regular_gate(flow,job.context['work_cycle'],anchor):return False
    _,_,canonical=ready_audit_view(lane,audit,anchor)
    if _ready_decline(audit,canonical)is not None:
        # An exact canonical commitment can precede its local receipt callback.
        # It only permits computation; finish still waits for that exact receipt.
        bound=(canonical and canonical.get('actor')==audit.get('actor')
               and canonical.get('ranges')==audit['ranges']and canonical.get('expired')is False)
        if not(bound and canonical.get('state')in('committed','passed','failed')
               and canonical.get('commitment')==audit['answer']['commitment']):return False
    # Recheck reservation/hot binding after all disk/Core reads.
    again=candidate(flow,job)
    if (again is None or again[2]!=ref
            or flow.worker.rpc.call('getbestblockhash')!=anchor):return False
    job.context['extra_publication_overlap']=ref
    return True


def _park(flow,job,result,status):
    work=job.context['work'];validate_computed_report(result,work)
    key=hashlib.sha256(canonical_bytes([work['request_id'],work['ticket']])).hexdigest()
    folder=flow.worker.root/'speculative-search-retained'/key;folder.mkdir(parents=True,exist_ok=True)
    payload=canonical_bytes(dict(schema='PGN_SPECULATIVE_RESULT_RETAINED_V1',work=work,
        result=result,extra=job.context['extra_publication_overlap']))+b'\n'
    if len(payload)>262144:raise BufferError('Speculative retention exceeds budget')
    file=folder/'result.json'
    if file.exists():
        if file.stat().st_size>262144 or file.read_bytes()!=payload:raise ValueError('Immutable speculative result changed')
    else:atomic_bytes(file,payload)
    atomic_bytes(folder/'last-observation.json',canonical_bytes(status)+b'\n')


def _repair(flow,job,result,status):
    """Reuse the prepared allocator's existing expiry/reassignment recovery."""
    _park(flow,job,result,status);work=job.context['work'];gpu=flow.gpu
    # Native execution has returned. Temporarily release this completed queue
    # identity so the existing unstarted-assignment repair can prove its handoff.
    with gpu.condition:
        if gpu.jobs.get(job.ticket['ticket_id'])is not job:return False
        gpu.jobs.pop(job.ticket['ticket_id'])
    repaired=False
    try:
        with flow.worker.network_lock:
            journal=flow.allocator.root/'controller.json'
            from horizon_mining_flow import read
            saved=read(journal,262144)
            if status['status']=='reassigned':
                queue=flow.reassignments
                with queue.lock:
                    prior=None
                    for key in queue.read():
                        record=queue.record(key)
                        if record['previous']['request_id']==work['request_id']and record['previous']['ticket']==work['ticket']:
                            prior=record;break
                    if prior is not None:queue.finish(prior);repaired=True
                    elif saved.get('work')==work and saved.get('stage')=='ASSIGNED':
                        repaired=queue.stage_unstarted(work,allocator_journal=saved)
            elif status['status']=='absent':
                if (saved.get('stage')=='LEASE_ENDED'and saved.get('previous_request')==work['request_id']
                        and saved.get('local_work_retained')is True):repaired=True
                elif saved.get('work')==work and saved.get('stage')=='ASSIGNED':
                    if flow.allocator.assign(recover_only=True)is None:
                        ended=read(journal,262144)
                        repaired=(ended.get('stage')=='LEASE_ENDED'and ended.get('previous_request')==work['request_id']
                                  and ended.get('local_work_retained')is True)
    finally:
        with gpu.condition:
            if not repaired:gpu.jobs[job.ticket['ticket_id']]=job
    if not repaired:return False
    with flow.prefetch.condition:
        if flow.prefetch.ready and flow.prefetch.ready['ticket']==work['ticket']:
            flow.prefetch.ready=None;flow.prefetch.condition.notify_all()
    with gpu.condition:
        job.retired=True
        if not job.future.done():job.future.set_exception(CancelledError('Original speculative result retained for recovery'))
        gpu.condition.notify_all()
    flow.worker.source.clear_cycle()
    return True


def _extra_gate(flow,cycle,anchor=None):
    """Do not promote from a stale derived extra-completion cache after reorg."""
    physical=flow.physical_audits;rpc=flow.worker.rpc
    observed=rpc.call('getbestblockhash')
    if anchor is not None and observed!=anchor:return False
    anchor=observed
    empty=physical.native.get(cycle,{}).get('empty')
    if empty:return (flow.audit_reference_current(dict(stage='unavailable',anchor=empty))
                     and rpc.call('getbestblockhash')==anchor)
    with physical.lock:done=deepcopy(physical.extras.get(cycle,{}))
    reserved=flow.cycles.extra_audits(cycle)
    if not reserved:return False
    for audit_id in reserved:
        block=done.get(audit_id)
        if not block:return False
        if flow.audit_reference_current(dict(stage='committed',block=block)):continue
        if rpc.call('getbestblockhash')!=anchor:return False
        header=rpc.call('getblockheader',[block,True])
        if (header.get('hash')==block and type(header.get('height'))is int and header['height']>=0
            and type(header.get('confirmations'))is int and header['confirmations']<0
            and rpc.call('getblockhash',[header['height']])!=block
            and rpc.call('getbestblockhash')==anchor):
            with physical.lock:
                current=deepcopy(physical.extras)
                if current.get(cycle,{}).get(audit_id)==block:
                    current[cycle].pop(audit_id)
                    if not current[cycle]:current.pop(cycle)
                    physical.save_extras(current)
        return False
    return rpc.call('getbestblockhash')==anchor


def finish(flow,job,result):
    """Return False to retry a durable result; never rerun its GPU computation."""
    if not flow.physical_ready(job.context['work_cycle']):
        # Existing queue retry deadline paces Core reads while publication waits.
        job.storage_retry_after=max(job.storage_retry_after,time.monotonic()+1.0)
    try:status=ticket_status(flow,job.context['work'])
    except (OSError,RuntimeError,ValueError)as error:
        flow.worker.network_notice='Speculative result retained: '+str(error)[:250]
        return False
    if status['status']in('absent','reassigned'):
        try:_repair(flow,job,result,status)
        except (OSError,RuntimeError,ValueError)as error:
            flow.worker.network_notice='Speculative recovery will retry: '+str(error)[:250]
        return False
    if status['status']!='same'or not flow.physical_ready(job.context['work_cycle']):return False
    try:
        if not _regular_gate(flow,job.context['work_cycle'],status['anchor'])or not _extra_gate(flow,job.context['work_cycle'],status['anchor']):return False
        if flow.worker.rpc.call('getbestblockhash')!=status['anchor']:return False
    except (OSError,RuntimeError,ValueError)as error:
        flow.worker.network_notice='Extra confirmation will retry: '+str(error)[:250]
        return False
    job.context['extra_publication_promotion']=True
    try:flow.activate(job)  # Same ticket; normal promotion/CPU grant only now.
    finally:job.context.pop('extra_publication_promotion',None)
    job.context.pop('extra_publication_overlap')
    atomic_bytes(flow.worker.root/'gpu-active.json',canonical_bytes(dict(ticket_id=job.ticket['ticket_id'],
        sequence=job.view_sequence,role='search',context=job.context))+b'\n')
    return True
