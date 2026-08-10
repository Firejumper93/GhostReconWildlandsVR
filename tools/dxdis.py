import pefile, struct, ctypes, sys
# 2026-08 update renamed this container: the _2015_ segment was dropped. Keep the
# old name as a fallback so the tool still works against a pre-update install.
# NOTE: this container holds post-process/terrain/water shaders only. MATERIAL
# shaders (SHD_Weapon_InGame, SHD_Basic) live in the .forge data and come out via
# Ubisoft_DATA_Tool.exe: see docs/RE-notes.md, "HOW A WEAPON IS ACTUALLY DRAWN".
import os as _os
_BASE = r'C:\Steam\steamapps\common\Wildlands'
DLL = _os.path.join(_BASE, 'shadercontainer_engine_win64_f.dll')
if not _os.path.exists(DLL):
    DLL = _os.path.join(_BASE, 'shadercontainer_engine_win64_2015_f.dll')
pe=pefile.PE(DLL,fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
exp={e.name.decode():e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
data=open(DLL,'rb').read(); ib=pe.OPTIONAL_HEADER.ImageBase
def blob(name):
    off=pe.get_offset_from_rva(exp[name])
    va=struct.unpack_from('<Q',data,off)[0]
    boff=pe.get_offset_from_rva(va-ib)
    assert data[boff:boff+4]==b'DXBC', data[boff:boff+4].hex()
    total=struct.unpack_from('<I',data,boff+24)[0]
    return data[boff:boff+total]
d3dc=ctypes.WinDLL(r'C:\Steam\steamapps\common\Wildlands\d3dcompiler_47.dll')
D=d3dc.D3DDisassemble
D.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_uint,ctypes.c_char_p,ctypes.POINTER(ctypes.c_void_p)]
D.restype=ctypes.c_long
def dis(b):
    out=ctypes.c_void_p()
    hr=D(b,len(b),0,None,ctypes.byref(out))
    if hr: return 'HRESULT 0x%08X'%(hr&0xffffffff)
    vt=ctypes.cast(out,ctypes.POINTER(ctypes.c_void_p))
    vtbl=ctypes.cast(vt[0],ctypes.POINTER(ctypes.c_void_p))
    GP=ctypes.CFUNCTYPE(ctypes.c_void_p,ctypes.c_void_p)(vtbl[3])
    GS=ctypes.CFUNCTYPE(ctypes.c_size_t,ctypes.c_void_p)(vtbl[4])
    return ctypes.string_at(GP(out),GS(out)).decode('utf-8','replace')
for name in sys.argv[1:]:
    b=blob(name); print('='*15,name,'bytes',len(b)); print(dis(b))
