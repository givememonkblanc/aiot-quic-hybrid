"""Pack the current model losslessly; never substitute weights or metadata."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import zlib


def pack(source, metadata, destination):
    text=source.read_text()
    match=re.search(r'g_vehicle_fomo_model_data\[\]\s*=\s*\{(.*?)\};',text,re.S)
    if not match:raise ValueError('Expected model byte array')
    data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b',match[1]))
    header=metadata.read_text()
    expected=int(re.search(r'\.len\s*=\s*(\d+)U?',header)[1])
    if len(data)!=expected or data[4:8]!=b'TFL3':raise ValueError('Model metadata does not match weights')
    def field(table, index):
        vt=table-struct.unpack_from('<i',data,table)[0]
        size=struct.unpack_from('<H',data,vt)[0]
        off=struct.unpack_from('<H',data,vt+4+index*2)[0] if 4+index*2<size else 0
        return table+off if off else None
    root=struct.unpack_from('<I',data,0)[0];ptr=field(root,1)
    vector=ptr+struct.unpack_from('<I',data,ptr)[0]
    count=struct.unpack_from('<I',data,vector)[0]
    methods={0:'Add',1:'AveragePool2D',2:'Concatenation',3:'Conv2D',4:'DepthwiseConv2D',
             9:'FullyConnected',18:'Mul',19:'Relu',22:'Reshape',25:'Softmax',34:'Pad',
             40:'Mean',41:'Sub',57:'Minimum'}
    registrations=[]
    for index in range(count):
        ptr=vector+4+4*index;table=ptr+struct.unpack_from('<I',data,ptr)[0]
        full=field(table,3);legacy=field(table,0)
        code=struct.unpack_from('<i',data,full)[0] if full else data[legacy] if legacy else 0
        if code not in methods:raise ValueError(f'Unsupported model operator: {code}')
        registrations.append(f'if (!add_resolver_op(ctx->resolver.Add{methods[code]}(), "{methods[code]}")) return false;')
    compressed=zlib.compress(data,9)
    if zlib.decompress(compressed)!=data:raise ValueError('Lossless verification failed')
    # Build-local metadata retains every model parameter but fills data at runtime.
    header=re.sub(r'\.data\s*=\s*g_vehicle_fomo_model_data\s*,', '.data = NULL,', header)
    destination.mkdir(parents=True,exist_ok=True)
    files={'ai_model_compact.h':header,
        'ai_model_register_ops.inc':'\n'.join(registrations)+'\n',
        'ai_model_compact.c':'#include <stddef.h>\nconst unsigned char g_aiot_model_zlib[] = {\n'+
            '\n'.join(','.join(f'0x{x:02x}' for x in compressed[i:i+32])+',' for i in range(0,len(compressed),32))+
            '\n};\nconst size_t g_aiot_model_zlib_len = sizeof(g_aiot_model_zlib);\n'}
    for name,contents in files.items():
        path=destination/name
        if not path.exists() or path.read_text()!=contents:path.write_text(contents)
    print('Model lossless pack:',len(data),'->',len(compressed),'SHA256',hashlib.sha256(data).hexdigest())

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('metadata',type=Path);p.add_argument('destination',type=Path)
    a=p.parse_args();pack(a.source,a.metadata,a.destination)
