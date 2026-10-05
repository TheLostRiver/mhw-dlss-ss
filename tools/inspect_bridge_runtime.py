"""Read-only preflight for the bridge's additional MHWSS callback/NGX sites."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('pid',type=int)
parser.add_argument('modules',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args()
if args.modules.resolve()==args.output.resolve():parser.error('Output cannot replace input.')
modules=json.loads(args.modules.read_text('utf-8-sig'))
kernel=ctypes.WinDLL('kernel32',use_last_error=True)
kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
kernel.OpenProcess.restype=wintypes.HANDLE
kernel.ReadProcessMemory.argtypes=[wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
kernel.ReadProcessMemory.restype=wintypes.BOOL
kernel.CloseHandle.argtypes=[wintypes.HANDLE]
handle=kernel.OpenProcess(0x0410,False,args.pid)
if not handle:raise ctypes.WinError(ctypes.get_last_error())
def read(address,size):
    buf=ctypes.create_string_buffer(size);used=ctypes.c_size_t()
    if not kernel.ReadProcessMemory(handle,address,buf,size,ctypes.byref(used)) or used.value!=size:raise ctypes.WinError(ctypes.get_last_error())
    return buf.raw
def module_at(address):
    found=next((m for m in modules if m['base']<=address<m['base']+m['size']),None)
    return {'name':found['name'],'rva':hex(address-found['base']),'path':found['path']} if found else None
try:
    mhwss=next(m for m in modules if m['name'].lower()=='mhwss.dll')
    assert hashlib.sha256(Path(mhwss['path']).read_bytes()).hexdigest()=='55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a'
    signatures={
        0xf0b40:'48895c2408574883ec20488b024c8d4c',
        0xf1360:'48895c240848896c2418488974242057',
        0x301aed:'488b1d2c802500',
        0x3019c3:'488b1d5e812500',
    }
    checked=[]
    for rva,signature in signatures.items():
        actual=read(mhwss['base']+rva,len(bytes.fromhex(signature))).hex()
        checked.append({'rva':hex(rva),'expected':signature,'actual':actual,'matches':actual==signature})
    additional={
        'monsterhunterworld.exe':{
            0x229ed30:'40534883ec2080b9f4b3070000488bd9',
            0x2297e20:'488bc448895808488970104889781855',
            0x23c3790:'44894424184889542410555341544155',
            0x23d0330:'405553488d6c24e84881ec180100008b',
        },
        'd3d12core.dll':{
            0x12ae40:'4883ec284c8bd9488b41584c8b15aea1',
            0x131010:'48895c241048896c2418488974242057',
        },
    }
    for name,targets in additional.items():
        module=next(m for m in modules if m['name'].lower()==name)
        for rva,signature in targets.items():
            actual=read(module['base']+rva,len(bytes.fromhex(signature))).hex()
            checked.append({'module':name,'rva':hex(rva),'expected':signature,'actual':actual,'matches':actual==signature})
    slots=[]
    for name,rva in [('create',0x559af8),('evaluate',0x559b00),('release',0x559b08),('allocate',0x559b18),('capabilities',0x559b20),('destroy',0x559b28)]:
        address=struct.unpack('<Q',read(mhwss['base']+rva,8))[0]
        slots.append({'name':name,'slot_rva':hex(rva),'address':hex(address),'owner':module_at(address)})
    flags=read(mhwss['base']+0x54ebe0,4)
    psos=struct.unpack('<QQ',read(mhwss['base']+0x54ec00,16))
    result={'pid':args.pid,'access':'QUERY_INFORMATION | VM_READ','writes_process_memory':False,
        'signatures':checked,'ngx_slots':slots,'taa_flags':list(flags),'taa_psos':[hex(p) for p in psos]}
    args.output.write_text(json.dumps(result,indent=2),'utf-8')
    print(json.dumps(result,indent=2))
finally:kernel.CloseHandle(handle)
