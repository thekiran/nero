#!/usr/bin/env python3
"""Encode PTY replay frames as a silent Motion JPEG QuickTime movie using Pillow.
No platform video service or additional codec is needed.
"""
from io import BytesIO
from pathlib import Path
import struct
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
W,H,FPS=1152,720,8
u16=lambda n:struct.pack('>H',n)
u32=lambda n:struct.pack('>I',n)
atom=lambda name,data: u32(len(data)+8)+name.encode()+data
full=lambda name,data,flags=0: atom(name,u32(flags)+data)
identity=struct.pack('>9I',0x10000,0,0,0,0x10000,0,0,0,0x40000000)
frames=[]
for p in sorted((ROOT/'.build/demo-frames').glob('*.png')):
 with Image.open(p) as im:
  buf=BytesIO(); im.resize((W,H),Image.Resampling.LANCZOS).save(buf,format='JPEG',quality=62,optimize=True)
  frames.append(buf.getvalue())
assert frames
n=len(frames)
ftyp=atom('ftyp',b'qt  '+u32(0)+b'qt  ')
mdat=atom('mdat',b''.join(frames))
offsets=[]; pos=len(ftyp)+8
for frame in frames: offsets.append(pos); pos+=len(frame)
mvhd=full('mvhd',u32(0)*2+u32(FPS)+u32(n)+u32(0x10000)+u16(0x100)+bytes(10)+identity+bytes(24)+u32(2))
tkhd=full('tkhd',u32(0)*2+u32(1)+u32(0)+u32(n)+bytes(8)+u16(0)*4+identity+u32(W<<16)+u32(H<<16),3)
mdhd=full('mdhd',u32(0)*2+u32(FPS)+u32(n)+u16(0)*2)
hdlr=full('hdlr',u32(0)+b'vide'+bytes(12)+b'NERO Terminal Replay\x00')
vmhd=full('vmhd',bytes(8),1)
dinf=atom('dinf',full('dref',u32(1)+full('url ',b'',1)))
name=b'NERO PTY Replay'
visual=bytes(6)+u16(1)+u16(0)*2+bytes(12)+u16(W)+u16(H)+u32(72<<16)*2+u32(0)+u16(1)+bytes([len(name)])+name+bytes(31-len(name))+u16(24)+u16(0xffff)
stsd=full('stsd',u32(1)+atom('jpeg',visual))
stts=full('stts',u32(1)+u32(n)+u32(1))
stsc=full('stsc',u32(1)+u32(1)*3)
stsz=full('stsz',u32(0)+u32(n)+b''.join(u32(len(f)) for f in frames))
stco=full('stco',u32(n)+b''.join(u32(o) for o in offsets))
stbl=atom('stbl',stsd+stts+stsc+stsz+stco)
moov=atom('moov',mvhd+atom('trak',tkhd+atom('mdia',mdhd+hdlr+atom('minf',vmhd+dinf+stbl))))
path=ROOT/'docs/media/nero-demo.mov'
path.write_bytes(ftyp+mdat+moov)
print(f'{path.name}: {W}x{H}, {n/FPS:.3f}s, {path.stat().st_size:,} bytes')
