#!/usr/bin/env python3
# Heuristic decoder for DCP fw binary log buffer (os_log-like arg encoding).
import sys,struct
d=open(sys.argv[1],'rb').read()
def tryparse(p):
    if p+2>len(d) or d[p]!=2: return None
    n=d[p+1]
    if n==0 or n>16: return None
    q=p+2; args=[]; slen=0
    for _ in range(n):
        if q+2>len(d): return None
        desc,sz=d[q],d[q+1]; q+=2
        if sz not in (1,2,4,8) or q+sz>len(d): return None
        v=d[q:q+sz]; q+=sz
        if desc & 0xf0 == 0x20:
            if sz!=4: return None
            o,l=struct.unpack('<HH',v)
            if o!=slen or l>200: return None
            slen+=l; args.append(('s',o,l))
        elif desc & 0xf0 in (0x00,0x10):
            args.append(('i',int.from_bytes(v,'little'),sz))
        else: return None
    if q+slen>len(d): return None
    blob=d[q:q+slen]
    out=[]
    for a in args:
        if a[0]=='s':
            s=blob[a[1]:a[1]+a[2]]
            if not all(32<=c<127 for c in s): return None
            out.append(s.decode())
        else:
            out.append(hex(a[1]))
    if not any(a[0]=='s' for a in args): return None
    return q+slen,out
p=0
while p<len(d):
    r=tryparse(p)
    if r:
        end,out=r
        print(f'{p:05x}', ' '.join(out))
        p=end
    else: p+=1
