import argparse, ctypes as c, ctypes.wintypes as w, json, subprocess, sys, time, shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("--cwd",required=True);p.add_argument("--receipt",required=True);p.add_argument("--timeout",type=int,default=300);p.add_argument("command",nargs=argparse.REMAINDER);a=p.parse_args()
cmd=a.command[1:] if a.command and a.command[0]=="--" else a.command
k=c.WinDLL("kernel32",use_last_error=True)
class Basic(c.Structure):
    _fields_=[("ProcessTime",c.c_longlong),("JobTime",c.c_longlong),("Flags",w.DWORD),("MinWorking",c.c_size_t),("MaxWorking",c.c_size_t),("Active",w.DWORD),("Affinity",c.c_size_t),("Priority",w.DWORD),("Scheduling",w.DWORD)]
class IO(c.Structure):_fields_=[(s,c.c_ulonglong) for s in ("ReadOps","WriteOps","OtherOps","ReadBytes","WriteBytes","OtherBytes")]
class Limits(c.Structure):
    _fields_=[("Basic",Basic),("IO",IO),("ProcessMemory",c.c_size_t),("JobMemory",c.c_size_t),("PeakProcess",c.c_size_t),("PeakJob",c.c_size_t)]
class Startup(c.Structure):
    _fields_=[("cb",w.DWORD),("reserved",w.LPWSTR),("desktop",w.LPWSTR),("title",w.LPWSTR)]+[(s,w.DWORD) for s in ("x","y","width","height","charsX","charsY","fill","flags")]+[("show",w.WORD),("reservedSize",w.WORD),("reservedBytes",c.POINTER(w.BYTE)),("stdin",w.HANDLE),("stdout",w.HANDLE),("stderr",w.HANDLE)]
class Process(c.Structure):_fields_=[("process",w.HANDLE),("thread",w.HANDLE),("pid",w.DWORD),("tid",w.DWORD)]
k.CreateJobObjectW.argtypes=[c.c_void_p,w.LPCWSTR];k.CreateJobObjectW.restype=w.HANDLE
k.GetCurrentProcess.restype=w.HANDLE
k.GetProcessAffinityMask.argtypes=[w.HANDLE,c.POINTER(c.c_size_t),c.POINTER(c.c_size_t)]
k.SetInformationJobObject.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.DWORD]
k.QueryInformationJobObject.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.DWORD,c.c_void_p]
k.GetStdHandle.argtypes=[w.DWORD];k.GetStdHandle.restype=w.HANDLE
k.SetHandleInformation.argtypes=[w.HANDLE,w.DWORD,w.DWORD]
k.CreateProcessW.argtypes=[w.LPCWSTR,w.LPWSTR,c.c_void_p,c.c_void_p,w.BOOL,w.DWORD,c.c_void_p,w.LPCWSTR,c.POINTER(Startup),c.POINTER(Process)]
k.AssignProcessToJobObject.argtypes=[w.HANDLE,w.HANDLE]
k.ResumeThread.argtypes=[w.HANDLE];k.ResumeThread.restype=w.DWORD
k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD];k.WaitForSingleObject.restype=w.DWORD
k.GetExitCodeProcess.argtypes=[w.HANDLE,c.POINTER(w.DWORD)]
k.TerminateJobObject.argtypes=[w.HANDLE,w.UINT]
k.CloseHandle.argtypes=[w.HANDLE]
def require(value):
    if not value:raise c.WinError(c.get_last_error())
affinity=c.c_size_t();system=c.c_size_t();require(k.GetProcessAffinityMask(k.GetCurrentProcess(),c.byref(affinity),c.byref(system)))
bits=[1<<n for n in range(64) if affinity.value&(1<<n)][:2];mask=sum(bits)
job=k.CreateJobObjectW(None,None);require(job)
limits=Limits();limits.Basic.Flags=0x2000|0x200|0x10|0x20;limits.Basic.Priority=0x40;limits.Basic.Affinity=mask;limits.JobMemory=2_684_354_560
require(k.SetInformationJobObject(job,9,c.byref(limits),c.sizeof(limits)))
si=Startup();si.cb=c.sizeof(si);si.flags=0x100
for name,number in (("stdin",-10),("stdout",-11),("stderr",-12)):
    handle=k.GetStdHandle(number&0xffffffff);setattr(si,name,handle)
    if handle and handle!=c.c_void_p(-1).value:require(k.SetHandleInformation(handle,1,1))
pi=Process();started=time.time()
require(k.CreateProcessW(None,c.create_unicode_buffer(subprocess.list2cmdline(cmd)),None,None,True,0x4|0x08000000,None,a.cwd,c.byref(si),c.byref(pi)))
try:
    require(k.AssignProcessToJobObject(job,pi.process));require(k.ResumeThread(pi.thread)!=0xffffffff)
    reason=None
    while True:
        wait=k.WaitForSingleObject(pi.process,1000)
        if wait!=258:break
        if shutil.disk_usage('C:/').free<8*1024**3:reason='C-reserve-below8GiB';break
        if time.time()-started>a.timeout:reason='timeout';break
    timed_out=reason is not None
    if timed_out:require(k.TerminateJobObject(job,124));k.WaitForSingleObject(pi.process,10000)
    code=w.DWORD();require(k.GetExitCodeProcess(pi.process,c.byref(code)))
    observed=Limits();require(k.QueryInformationJobObject(job,9,c.byref(observed),c.sizeof(observed),None))
    record=dict(priority_class="IDLE",C_reserve_bytes=8*1024**3,C_free_bytes=shutil.disk_usage("C:/").free,stop_reason=reason,command=cmd,cwd=a.cwd,job_memory_limit_bytes=limits.JobMemory,peak_job_memory_bytes=observed.PeakJob,logical_cpu_count=len(bits),affinity_mask=mask,timeout=timed_out,exit_code=code.value,elapsed_seconds=round(time.time()-started,3))
    Path(a.receipt).write_text(json.dumps(record,indent=2),encoding="utf-8");print("BOUNDED WINDOWS RECEIPT",json.dumps(record),flush=True)
finally:
    for handle in (pi.thread,pi.process,job):k.CloseHandle(handle)
sys.exit(code.value)
