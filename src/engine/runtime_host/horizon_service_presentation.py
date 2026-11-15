"""Read-only presentation of audits performed before a search ticket is issued.

This module neither grants service credits nor decides execution. The owning
Core remains authoritative; absent observations are represented as unknown.
"""
import re


def identity(value):
    return type(value) is str and re.fullmatch('[0-9a-f]{64}', value) is not None and value != '0'*64


def audit_key(value):
    if (not isinstance(value, dict) or not identity(value.get('request_id'))
            or type(value.get('slot')) is not int or value['slot'] not in (0, 1)
            or type(value.get('attempt')) is not int or not 0 <= value['attempt'] < 1 << 32):
        raise ValueError('Malformed observed audit reference')
    return value['request_id'], value['slot'], value['attempt']


def preparation_view(context, status):
    """Return a view only for an actual audit without an assigned search cycle."""
    if (context.get('role') != 'audit' or context.get('work_cycle')
            or context.get('audit_budget_cycle')):
        return None
    active = context.get('presentation', {}).get('audit_id')
    if not identity(active):
        return None
    try:
        current = audit_key(dict(request_id=context.get('audit_request_id'),
                                 slot=context.get('audit_slot'), attempt=context.get('audit_attempt')))
    except ValueError:
        return None
    view = dict(schema='PGN_PRE_TICKET_AUDIT_VIEW_V1', active_audit_id=active,
                search_assigned=False, confirmed_service_answers=None, active_index=None)
    if status is None:
        return view
    if (not isinstance(status, dict) or status.get('schema') != 'PGN_NATIVE_WORK_DUTY_STATUS_V1'
            or status.get('enforced') is not True or not identity(status.get('bestblockhash'))
            or type(status.get('may_request')) is not bool
            or type(status.get('credits')) is not list or len(status['credits']) > 2):
        return view
    try:
        keys=[]
        for credit in status['credits']:
            key=audit_key(credit)
            if not identity(credit.get('commitment')) or key in keys:
                raise ValueError('Invalid or duplicate observed service answer')
            keys.append(key)
    except ValueError:
        return view
    if current not in keys and len(keys) == 2:
        return view  # Do not invent a third pre-ticket slot or a completed audit.
    view['confirmed_service_answers']=len(keys)
    view['active_index']=keys.index(current) if current in keys else len(keys)
    return view
