"""Explicit developer control of the standalone ToolbarIcons prototype (Windows x64).

Only touches the specified running Designer, validates its executable and DLL
identity, and never retries remote calls with unknown completion status.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import subprocess

K = C.WinDLL("kernel32",use_last_error=True)
HANDLE = W.HANDLE
PTR = C.c_void_p
SIZE = C.c_size_t
SIGNATURES = {
    "OpenProcess": ([W.DWORD,W.BOOL,W.DWORD],HANDLE),
    "CloseHandle": ([HANDLE],W.BOOL),
    "CreateToolhelp32Snapshot": ([W.DWORD,W.DWORD],HANDLE),
    "Module32FirstW": ([HANDLE,PTR],W.BOOL), "Module32NextW": ([HANDLE,PTR],W.BOOL),
    "VirtualAllocEx": ([HANDLE,PTR,SIZE,W.DWORD,W.DWORD],PTR),
    "VirtualFreeEx": ([HANDLE,PTR,SIZE,W.DWORD],W.BOOL),
    "WriteProcessMemory": ([HANDLE,PTR,PTR,SIZE,C.POINTER(SIZE)],W.BOOL),
    "CreateRemoteThread": ([HANDLE,PTR,SIZE,PTR,PTR,W.DWORD,PTR],HANDLE),
    "WaitForSingleObject": ([HANDLE,W.DWORD],W.DWORD),
    "GetExitCodeThread": ([HANDLE,C.POINTER(W.DWORD)],W.BOOL),
    "GetModuleHandleW": ([W.LPCWSTR],HANDLE),
    "GetProcAddress": ([HANDLE,C.c_char_p],PTR),
    "GetModuleHandleExW": ([W.DWORD,PTR,C.POINTER(HANDLE)],W.BOOL),
    "GetModuleFileNameW": ([HANDLE,W.LPWSTR,W.DWORD],W.DWORD),
    "LoadLibraryExW": ([W.LPCWSTR,HANDLE,W.DWORD],HANDLE),
    "FreeLibrary": ([HANDLE],W.BOOL),
}
for name,(arguments,result) in SIGNATURES.items():
    getattr(K,name).argtypes=arguments
    getattr(K,name).restype=result


class Module(C.Structure):
    _fields_=[("size",W.DWORD),("module",W.DWORD),("pid",W.DWORD),("global_usage",W.DWORD),
              ("process_usage",W.DWORD),("base",PTR),("length",W.DWORD),("handle",HANDLE),
              ("name",W.WCHAR*256),("path",W.WCHAR*260)]


def check(value,label):
    if not value:
        raise C.WinError(C.get_last_error(),label)
    return value


def modules(pid):
    snapshot=K.CreateToolhelp32Snapshot(0x18,pid)
    if snapshot==PTR(-1).value:
        raise C.WinError(C.get_last_error())
    result={}
    try:
        entry=Module(); entry.size=C.sizeof(entry)
        ok=K.Module32FirstW(snapshot,C.byref(entry))
        while ok:
            result[entry.name.lower()]=(entry.base,Path(entry.path))
            ok=K.Module32NextW(snapshot,C.byref(entry))
    finally:
        K.CloseHandle(snapshot)
    return result


def remote_call(process,address,argument=None):
    allocation=None
    if argument is not None:
        allocation=check(K.VirtualAllocEx(process,None,len(argument),0x3000,4),"VirtualAllocEx")
        written=SIZE()
        if not K.WriteProcessMemory(process,allocation,argument,len(argument),C.byref(written)) or written.value!=len(argument):
            K.VirtualFreeEx(process,allocation,0,0x8000)
            raise C.WinError(C.get_last_error())
    thread=K.CreateRemoteThread(process,None,0,address,allocation,0,None)
    if not thread:
        if allocation:
            K.VirtualFreeEx(process,allocation,0,0x8000)
        raise C.WinError(C.get_last_error())
    try:
        if K.WaitForSingleObject(thread,15000)!=0:
            raise RuntimeError("Remote call outcome unknown; argument retained, do not retry")
        if allocation:
            K.VirtualFreeEx(process,allocation,0,0x8000)
        code=W.DWORD()
        check(K.GetExitCodeThread(thread,C.byref(code)),"GetExitCodeThread")
        return code.value
    finally:
        K.CloseHandle(thread)


def control(pid,dll,action,pack=None):
    # The command is constant apart from an argparse-validated integer PID.
    command=f"Get-CimInstance Win32_Process -Filter 'ProcessId={int(pid)}' | Select-Object ExecutablePath,CommandLine | ConvertTo-Json -Compress"
    info=json.loads(subprocess.check_output(["powershell.exe","-NoProfile","-Command",command],text=True,encoding="utf-8"))
    if Path(info["ExecutablePath"]).name.lower()!="1cv8.exe" or " DESIGNER " not in info["CommandLine"].upper():
        raise ValueError("The explicitly selected PID must be 1cv8.exe DESIGNER")
    dll=dll.resolve(strict=True)
    process=check(K.OpenProcess(0x43a,False,pid),"OpenProcess")
    try:
        loaded=modules(pid)
        if dll.name.lower() not in loaded:
            if action!="start":
                raise ValueError("Prototype is not loaded")
            loader=check(K.GetProcAddress(K.GetModuleHandleW("kernel32.dll"),b"LoadLibraryW"),"LoadLibraryW")
            owner=HANDLE()
            check(K.GetModuleHandleExW(6,loader,C.byref(owner)),"LoadLibrary owner")
            path=C.create_unicode_buffer(32768)
            check(K.GetModuleFileNameW(owner,path,len(path)),"LoadLibrary module path")
            remote_owner=loaded[Path(path.value).name.lower()][0]
            remote_call(process,remote_owner+loader-owner.value,(str(dll)+"\0").encode("utf-16le"))
            loaded=modules(pid)
        base,path=loaded[dll.name.lower()]
        if path.resolve()!=dll:
            raise ValueError("A different prototype DLL is already loaded; use a new test Designer")
        local=check(K.LoadLibraryExW(str(dll),None,1),"DLL metadata")
        try:
            name={"start":"ToolbarIconsStart","stop":"ToolbarIconsStop","snapshot":"ToolbarIconsSnapshot"}[action]
            export=check(K.GetProcAddress(local,name.encode()),name)
            argument=(str(pack.resolve(strict=True))+"\0").encode("utf-16le") if pack else None
            result=remote_call(process,base+export-local,argument)
            if result:
                raise OSError(result,f"{name}: Win32 error {result}")
            print(f"{name}: OK (PID {pid})")
        finally:
            K.FreeLibrary(local)
    finally:
        K.CloseHandle(process)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=("start","stop","snapshot"))
    parser.add_argument("--pid",type=int,required=True)
    parser.add_argument("--dll",type=Path,default=Path("build/native/toolbar-prototype/bin/prototype/ToolbarIcons.dll"))
    parser.add_argument("--pack",type=Path)
    args=parser.parse_args()
    if args.action=="start" and args.pack is None:
        parser.error("start requires --pack")
    control(args.pid,args.dll,args.action,args.pack)


if __name__=="__main__":
    main()
