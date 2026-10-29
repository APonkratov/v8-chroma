"""Preview an icon theme in one already attached Designer using its ABI-v2 session.

Developer QA helper. Changes only the explicitly selected process; it does not
change the manager's preferences or auto-apply setting. Calls the already loaded
engine and validates its saved identity and binary before applying a golden.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import struct

from toolbar_icons_control import K, modules, remote_call, check

ROOT=Path(__file__).resolve().parents[1]


def wide(value,capacity):
    encoded=str(value).encode('utf-16le')
    if '\0' in str(value) or len(encoded)>=capacity*2:
        raise ValueError('Invalid protocol path')
    return encoded.ljust(capacity*2,b'\0')


def apply(pid,session_path,theme):
    session=json.loads(session_path.read_text(encoding='utf-8'))
    identity=session['identity']
    if identity['pid']!=pid or session['schema_version']!=1 or session['stage']!='ready':
        raise ValueError('Session is not a ready session for this PID')
    loaded=modules(pid)
    base,dll=loaded['themeprobe.dll']
    if dll.resolve()!=Path(session['dll']).resolve() or loaded['1cv8.exe'][1].resolve()!=Path(identity['exe']).resolve():
        raise ValueError('Loaded process or theme DLL differs from session')
    if hashlib.sha256(dll.read_bytes()).hexdigest()!=session['dll_sha256']:
        raise ValueError('Saved DLL fingerprint differs')
    process=check(K.OpenProcess(0x43a,False,pid),'OpenProcess')
    K.GetProcessTimes.argtypes=[W.HANDLE,C.POINTER(W.FILETIME),C.POINTER(W.FILETIME),C.POINTER(W.FILETIME),C.POINTER(W.FILETIME)]
    K.GetProcessTimes.restype=W.BOOL
    try:
        times=[W.FILETIME() for _ in range(4)]
        check(K.GetProcessTimes(process,*[C.byref(value) for value in times]),'GetProcessTimes')
        created=(times[0].dwHighDateTime<<32)|times[0].dwLowDateTime
        if created!=identity['created_time']:
            raise ValueError('PID identity changed')
        source=ROOT/'tests/golden'/f'{theme}.bin'
        cfg=bytearray(source.read_bytes() if theme!='standard' else (ROOT/'tests/golden/github-light-default.bin').read_bytes())
        if len(cfg)!=4752 or struct.unpack_from('<II',cfg)!=(4752,2):
            raise ValueError('Expected protocol v2 golden')
        if theme=='standard':
            struct.pack_into('<II',cfg,8,0,0)
            cfg[16:80]=b'standard'.ljust(64,b'\0')
        cfg[120:640]=wide(Path(identity['exe']).parent,260)
        cfg[640:1680]=wide(session['log'],520)
        local=check(K.LoadLibraryExW(str(dll),None,1),'Theme metadata')
        try:
            address=check(K.GetProcAddress(local,b'ThemeApply'),'ThemeApply')
            result=remote_call(process,base+address-local,bytes(cfg))
            if result:
                raise OSError(result,'ThemeApply')
            snapshot=check(K.GetProcAddress(local,b'ThemeSnapshot'),'ThemeSnapshot')
            result=remote_call(process,base+snapshot-local)
            if result:
                raise OSError(result,'ThemeSnapshot')
        finally:
            K.FreeLibrary(local)
    finally:
        K.CloseHandle(process)
    log=Path(session['log']).read_text(encoding='utf-8')
    if f'# theme={theme}\n' not in log:
        raise ValueError('Snapshot did not confirm theme')
    redraw(pid)
    print(f'Theme preview {theme}: OK (PID {pid})')


def redraw(pid):
    """Use the same theme notification as the project's manager::attach path."""
    user=C.WinDLL('user32',use_last_error=True)
    callback=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    user.EnumWindows.argtypes=[callback,W.LPARAM]
    user.EnumChildWindows.argtypes=[W.HWND,callback,W.LPARAM]
    user.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
    user.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
    user.RedrawWindow.argtypes=[W.HWND,C.c_void_p,W.HANDLE,W.UINT]
    @callback
    def child(window,_):
        user.PostMessageW(window,0x31a,0,0)
        return True
    @callback
    def visit(window,_):
        owner=W.DWORD()
        user.GetWindowThreadProcessId(window,C.byref(owner))
        if owner.value==pid:
            user.PostMessageW(window,0x31a,0,0)
            user.EnumChildWindows(window,child,0)
            user.RedrawWindow(window,None,None,0x485)
        return True
    user.EnumWindows(visit,0)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int,required=True)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--theme',choices=['standard',*[p.stem for p in sorted((ROOT/'tests/golden').glob('*.bin'))]],required=True)
    args=parser.parse_args()
    apply(args.pid,args.session,args.theme)


if __name__=='__main__':
    main()
