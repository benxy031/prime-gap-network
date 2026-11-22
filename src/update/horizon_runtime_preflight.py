"""Start authenticated code in non-working modes before touching the live UI.

This is a loader check, not a mining/network health claim. The complete cohort
inventory is verified first; no caller-supplied command is accepted here.
"""
import os
from pathlib import Path
import subprocess
import time
from horizon_component_install import inventory
import stage_component_bundle as stage


def candidate_runtime_preflight(cohort,*,expected_inventory_sha256=None):
    cohort=Path(cohort)
    value,files,digest,_=inventory(cohort)
    if expected_inventory_sha256 is not None:
        stage.require(digest==expected_inventory_sha256,'Candidate runtime differs from authenticated preparation')
    suffix='.exe' if value.get('platform','windows-x86_64')=='windows-x86_64' else ''
    helper='UpdateRuntime/horizon-updater'+suffix
    descriptor='UpdateRuntime/deployment.json'
    stage.require(helper.casefold() in files and descriptor.casefold() in files,
        'Required update helper or deployment descriptor is missing')
    config=stage._json(stage._read_plain(cohort/descriptor,8192))
    stage.require(config.get('schema')=='PGN_UPDATE_DEPLOYMENT_V1'
        and config.get('network')==value['network'] and config.get('helper')==helper,
        'Candidate update deployment differs from its installed network or helper')
    stage.require(config.get('sha256')==files[helper.casefold()]['sha256'],
        'Candidate update helper fingerprint differs from its deployment')
    environment=dict(os.environ,PYINSTALLER_RESET_ENVIRONMENT='1')
    environment.pop('PGN_PRIVATE_OWNER_BOOTSTRAP',None)
    checks=[]
    for relative,argument in (
        ('EngineRuntime/network/horizon-engine-host'+suffix,'--help'),
        (helper,'--self-test'),
    ):
        stage.require(relative.casefold()in files,'Required runtime loader is missing')
        executable=cohort/relative
        started=time.monotonic()
        try:
            result=subprocess.run([str(executable),argument],cwd=str(cohort),
                env=environment,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,timeout=20,check=False,
                creationflags=(subprocess.CREATE_NO_WINDOW|subprocess.BELOW_NORMAL_PRIORITY_CLASS)if os.name=='nt'else 0)
        except (OSError,subprocess.TimeoutExpired)as error:
            raise stage.StageError('Candidate runtime cannot start before update: '+relative+' ('+type(error).__name__+')')from error
        stage.require(result.returncode==0,'Candidate runtime loader failed before update: '+relative+' (exit '+str(result.returncode)+')')
        checks.append(dict(path=relative,sha256=files[relative.casefold()]['sha256'],
            mode=argument,exit_code=0,seconds=round(time.monotonic()-started,3)))
    return dict(schema='PGN_CANDIDATE_RUNTIME_PREFLIGHT_V1',PASS=True,checks=checks,
        mining_started=False,live_processes_stopped=False)
