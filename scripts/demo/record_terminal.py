#!/usr/bin/env python3
"""Record the real NERO PTY output and render a shareable terminal replay.
Requires Pillow; uses only help/version/history/clear/exit, without network commands.
"""
import codecs
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import termios
import time
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'docs/media'
FRAMES = ROOT / '.build/demo-frames'
OUT.mkdir(exist_ok=True)
FRAMES.mkdir(parents=True, exist_ok=True)
COLS, ROWS = 110, 34
W, H = 1440, 900
FONT = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 19)
SMALL = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 17)
COLORS = ['#10151e','#f27583','#6edba2','#f2d07b','#77adff','#cf8fff','#70d5dc','#dce5f1']
grid = [[(' ',7) for _ in range(COLS)] for _ in range(ROWS)]
x=y=0
fg=7
pending=''
frames=[]
events=[]
decoder=codecs.getincrementaldecoder('utf-8')('replace')

def feed(data):
 global x,y,fg,pending,grid
 pending += decoder.decode(data)
 while pending:
  if pending[0]=='\x1b':
   m=re.match(r'\x1b\[([0-9;?]*)([ -/]*)([@-~])',pending)
   if not m:
    if len(pending)<2 or pending.startswith('\x1b['): return
    pending=pending[2:]; continue
   args,_,cmd=m.groups(); pending=pending[m.end():]
   nums=[int(v) if v else 0 for v in args.lstrip('?').split(';')]
   n=nums[0] or 1
   if cmd=='m':
    for n in nums:
     if n==0 or n==39: fg=7
     elif 30<=n<=37: fg=n-30
     elif 90<=n<=97: fg=n-90
   elif cmd in 'Hf': y=max(0,min(ROWS-1,n-1)); x=max(0,min(COLS-1,(nums[1] if len(nums)>1 else 1)-1))
   elif cmd=='J':
    if nums[0] in (2,3): grid=[[(' ',7) for _ in range(COLS)] for _ in range(ROWS)]
    else:
     grid[y][x:]=[(' ',7)]*(COLS-x)
     for row in range(y+1,ROWS): grid[row]=[(' ',7)]*COLS
   elif cmd=='K':
    start,end=(0,COLS) if nums[0]==2 else ((0,x+1) if nums[0]==1 else (x,COLS))
    grid[y][start:end]=[(' ',7)]*(end-start)
   elif cmd=='A': y=max(0,y-n)
   elif cmd=='B': y=min(ROWS-1,y+n)
   elif cmd=='C': x=min(COLS-1,x+n)
   elif cmd=='D': x=max(0,x-n)
   elif cmd=='G': x=max(0,min(COLS-1,n-1))
   continue
  ch,pending=pending[0],pending[1:]
  if ch=='\r': x=0
  elif ch=='\n':
   y+=1
   if y>=ROWS: grid.pop(0); grid.append([(' ',7)]*COLS); y=ROWS-1
  elif ch=='\b': x=max(0,x-1)
  elif ch=='\t': x=min(COLS-1,(x//8+1)*8)
  elif ord(ch)>=32:
   if x>=COLS:
    x=0; y+=1
    if y>=ROWS: grid.pop(0); grid.append([(' ',7)]*COLS); y=ROWS-1
   grid[y][x]=(ch,fg); x+=1

def render():
 im=Image.new('RGB',(W,H),'#0b1018'); d=ImageDraw.Draw(im)
 d.rounded_rectangle((18,18,W-18,H-18),radius=16,fill='#111925',outline='#293448',width=2)
 d.rounded_rectangle((19,19,W-19,68),radius=15,fill='#1c2737')
 d.rectangle((19,48,W-19,68),fill='#1c2737')
 for i,c in enumerate(('#ff6259','#ffbf2f','#2acb42')): d.ellipse((39+i*25,35,51+i*25,47),fill=c)
 d.text((150,29),'NERO  /  macOS ARM64  /  terminal session replay',font=SMALL,fill='#c1cddd')
 for row,line in enumerate(grid):
  for col,(ch,color) in enumerate(line):
   if ch!=' ': d.text((38+col*12.25,83+row*22),ch,font=FONT,fill=COLORS[color])
 d.rectangle((38+min(x,COLS-1)*12.25,84+y*22,40+min(x,COLS-1)*12.25,104+y*22),fill='#a7c7ff')
 d.text((39,858),'Live NERO output  |  help · version · history  |  No network commands',font=SMALL,fill='#8d9eb6')
 return im

master,slave=pty.openpty()
fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',ROWS,COLS,0,0))
env=dict(os.environ,TERM='xterm-256color',LC_ALL='en_US.UTF-8')
env.pop('NO_COLOR',None)
proc=subprocess.Popen([str(ROOT/'nero')],stdin=slave,stdout=slave,stderr=slave,cwd=ROOT,env=env,start_new_session=True)
os.close(slave)
start=time.monotonic()

def tick(seconds):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  deadline=time.monotonic()+0.125
  while time.monotonic()<deadline:
   ready,_,_=select.select([master],[],[],max(0,deadline-time.monotonic()))
   if ready:
    try: data=os.read(master,65536)
    except OSError: break
    if not data: break
    events.append([round(time.monotonic()-start,3),'o',data.decode('utf-8','replace')]); feed(data)
  frames.append(render())

def command(text,hold=2.0):
 for ch in text:
  os.write(master,ch.encode()); tick(.06)
 os.write(master,b'\r'); tick(hold)

try:
 tick(2)
 frames[-1].save(OUT/'nero-startup.png')
 command('help',3)
 frames[-1].save(OUT/'nero-help.png')
 command('version',2)
 command('history',3)
 frames[-1].save(OUT/'nero-history.png')
 command('history -clear',1.5)
 command('clear',1)
 command('help',2)
 command('exit',1)
 proc.wait(timeout=5)
 assert proc.returncode==0, proc.returncode
finally:
 if proc.poll() is None: proc.kill(); proc.wait()
 os.close(master)
with (OUT/'nero-demo.cast').open('w') as f:
 f.write(json.dumps({'version':2,'width':COLS,'height':ROWS,'title':'NERO terminal walkthrough','env':{'TERM':'xterm-256color'}})+'\n')
 for event in events: f.write(json.dumps(event)+'\n')
for i,im in enumerate(frames): im.save(FRAMES/f'{i:04d}.png')
frames[0].save(OUT/'nero-demo.gif',save_all=True,append_images=frames[1:],duration=125,loop=0,optimize=True)
print(f'Recorded {len(frames)} frames, {len(events)} output events, exit={proc.returncode}')
print(f'Replay duration: {len(frames)/8:.1f}s')
