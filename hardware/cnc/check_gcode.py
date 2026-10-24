"""Strict static checker for a restricted GRBL 1.1 PCB job (standard Python).

Checks the supplied G54 work envelope, including tool radius and arc extrema.
Does NOT prove fixture clearance, machine travel, spindle calibration, surface
mapping, cutting suitability, or the actual initial work offset. Rejects unknown
syntax rather than silently ignoring it. Supports absolute mm, XY I/J arcs only.
"""
import argparse
import json
import math
import re
from pathlib import Path

TOKEN = re.compile(r'([A-Z])([+-]?(?:\d+(?:\.\d*)?|\.\d+))')
GS = {0,1,2,3,4,17,21,40,49,54,80,90,91.1,94}
MS = {0,2,3,5,9,30}


def check(text, limits, start, radius=0, clearance=3, smax=10000,
          feed_xy=300, feed_z=120):
    pos=list(start); low=pos.copy(); high=pos.copy()
    mode=None; feed=None; speed=0; spindle=False; ended=False
    initialized=set(); tools=set(); pauses=0; moves=0
    def fail(n,message): raise ValueError(f'line {n}: {message}')
    def bounds(n,point):
        for i,(v,(a,b)) in enumerate(zip(point,limits)):
            margin=radius if i<2 else 0
            if not a-1e-6<=v-margin or not v+margin<=b+1e-6:
                fail(n,f'{"XYZ"[i]}={v:.5f}, tool radius {margin:g}, outside [{a:g},{b:g}]')
            low[i]=min(low[i],v); high[i]=max(high[i],v)
    bounds(0,pos)
    for n,raw in enumerate(text.splitlines(),1):
        clean=[]; depth=0
        for ch in raw:
            if ch==';' and depth==0: break
            if ch=='(': depth+=1; continue
            if ch==')':
                depth-=1
                if depth<0: fail(n,'unbalanced comment')
                continue
            if not depth: clean.append(ch)
        if depth: fail(n,'unclosed comment')
        line=''.join(clean).upper()
        compact=re.sub(r'\s+','',line)
        if not compact: continue
        if ended: fail(n,'commands after program end')
        if len(compact)>79: fail(n,'block exceeds conservative GRBL line length')
        if TOKEN.sub('',compact): fail(n,'unsupported token, expression or macro')
        pairs=[(k,float(v)) for k,v in TOKEN.findall(compact)]
        values={}; gc=[]; mc=[]
        for key,val in pairs:
            if not math.isfinite(val): fail(n,'non-finite number')
            if key=='G': gc.append(val)
            elif key=='M': mc.append(val)
            elif key in 'XYZFIJSTPN':
                if key in values: fail(n,f'duplicate {key}')
                values[key]=val
            else: fail(n,f'unsupported word {key}')
        if any(g not in GS for g in gc): fail(n,f'unsupported G code {gc}')
        if len(gc)!=len(set(gc)): fail(n,'duplicate G code')
        if any(m not in MS for m in mc): fail(n,f'unsupported M code {mc}; no M6/G81/G83')
        motions=[g for g in gc if g in {0,1,2,3,80}]
        if len(motions)>1 or len(mc)>1: fail(n,'conflicting modal commands')
        if 4 in gc and any(k in values for k in 'XYZIJ'): fail(n,'coordinates on dwell block')
        if 'P' in values and (4 not in gc or values['P']<0): fail(n,'invalid dwell')
        initialized.update(g for g in gc if g in {21,90,94,54})
        if motions: mode=None if motions[0]==80 else motions[0]
        if 'F' in values:
            if not {21,94}<=initialized: fail(n,'set G21/G94 before F')
            feed=values['F']
            if feed<=0: fail(n,'nonpositive feed')
        if 'S' in values:
            speed=values['S']
            if not 0<=speed<=smax: fail(n,'S exceeds declared command limit')
        if 'T' in values:
            tool=values['T']
            if tool<0 or tool!=int(tool): fail(n,'invalid tool number')
            if spindle: fail(n,'tool selection while spindle commanded on')
            tools.add(int(tool))
        if 3 in mc:
            if speed<=0: fail(n,'M3 without positive S')
            spindle=True
        if 5 in mc: spindle=False
        if 0 in mc:
            if spindle: fail(n,'M0 tool/setup pause requires explicit M5 first')
            pauses+=1
        has_axis=any(k in values for k in 'XYZ')
        arc=mode in {2,3} and ('I' in values or 'J' in values)
        if mode in {2,3} and has_axis and not arc: fail(n,'arc requires explicit I/J center')
        if ('I' in values or 'J' in values) and mode not in {2,3}:
            fail(n,'I/J outside an arc')
        if has_axis or arc:
            if not {21,90,94,54}<=initialized: fail(n,'missing explicit G21/G90/G94/G54')
            if mode is None: fail(n,'coordinates without motion mode')
            end=[values.get(k,pos[i]) for i,k in enumerate('XYZ')]
            xy=arc or end[:2]!=pos[:2]
            if mode==0:
                if xy and min(pos[2],end[2])<clearance-1e-6:
                    fail(n,'rapid XY below declared clearance')
                if end[2]<pos[2] and end[2]<0:
                    fail(n,'rapid down into stock')
            else:
                if feed is None: fail(n,'cutting motion without feed')
                if (xy and feed>feed_xy) or (end[2]!=pos[2] and feed>feed_z):
                    fail(n,'feed exceeds declared XY/Z limit')
                if min(pos[2],end[2])<0 and not spindle:
                    fail(n,'below-surface feed without M3')
            bounds(n,end)
            if arc:
                cx=pos[0]+values.get('I',0); cy=pos[1]+values.get('J',0)
                r=math.hypot(pos[0]-cx,pos[1]-cy)
                r2=math.hypot(end[0]-cx,end[1]-cy)
                if r<=1e-8 or abs(r-r2)>.01: fail(n,'invalid I/J arc radius')
                a=math.atan2(pos[1]-cy,pos[0]-cx)
                b=math.atan2(end[1]-cy,end[0]-cx)
                sign=1 if mode==3 else -1
                sweep=(sign*(b-a))%(2*math.pi)
                if math.dist(pos[:2],end[:2])<1e-8: sweep=2*math.pi
                for theta in [0,math.pi/2,math.pi,3*math.pi/2]:
                    if (sign*(theta-a))%(2*math.pi)<=sweep+1e-9:
                        for z in [pos[2],end[2]]:
                            bounds(n,[cx+r*math.cos(theta),cy+r*math.sin(theta),z])
            pos=end; moves+=1
        if 2 in mc or 30 in mc:
            if spindle or pos[2]<clearance: fail(n,'end requires M5 and retracted Z')
            ended=True
    if not ended or not moves: raise ValueError('missing program end or no motion')
    return {'status':'PASS_STATIC_SUBSET','bounds_center_mm':dict(zip('XYZ',zip(low,high))),
            'max_depth_below_nominal_zero_mm':max(0,-low[2]),'tools':sorted(tools),
            'pauses':pauses,'moves':moves,'tool_radius_mm':radius,
            'note':'Work coordinates only; fixture, offsets, height map and cutting unverified'}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('file',type=Path)
    for axis in 'xyz': ap.add_argument('--'+axis,nargs=2,type=float,required=True)
    ap.add_argument('--start',nargs=3,type=float,required=True,help='actual current G54 XYZ')
    ap.add_argument('--tool-radius',type=float,required=True)
    ap.add_argument('--xy-clearance',type=float,default=3)
    ap.add_argument('--smax',type=float,default=10000)
    ap.add_argument('--feed-xy-max',type=float,default=300)
    ap.add_argument('--feed-z-max',type=float,default=120)
    args=ap.parse_args()
    if args.tool_radius<0 or any(a>b for a,b in [args.x,args.y,args.z]): ap.error('invalid limits')
    try:
        report=check(args.file.read_text(encoding='utf-8-sig'),[args.x,args.y,args.z],
                     args.start,args.tool_radius,args.xy_clearance,args.smax,
                     args.feed_xy_max,args.feed_z_max)
    except ValueError as err: ap.exit(1,f'FAIL: {err}\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
