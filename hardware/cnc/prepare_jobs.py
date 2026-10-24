"""Generate review-only CNC 3018 jobs and 1:1 phototools from the real PCB.

Run with KiCad Python. All outputs need setup verification/coupon trials. Main
PCB is read only. Resist clearing is an EXPERIMENT for fully hardened etch resist,
not the normal UV expose/develop workflow and not direct 70 um copper milling.
"""
import csv
import hashlib
import json
import math
import re
from pathlib import Path
import pcbnew as p

ROOT=Path(__file__).resolve().parent
HW=ROOT.parent
WIDTH,HEIGHT=36.,60.
SAFE=5.


def header(title,tool,rpm=10000):
    return [f'({title})','(SETUP REQUIRED - NOT MACHINE VALIDATED)',
            '(VERIFY G54 Z0, CLEARANCE, RPM COMMAND AND TOOL)',
            'G17 G21 G40 G49 G54 G80 G90 G94','M5',f'T{tool}','M0',
            f'G0 Z{SAFE:.3f}',f'M3 S{rpm}','G4 P2']


def footer(): return [f'G0 Z{SAFE:.3f}','M5','M9','M30']


def nearest(points):
    remaining=list(points); ordered=[]; now=(0.,0.)
    while remaining:
        point=min(remaining,key=lambda v:math.dist(v,now))
        remaining.remove(point); ordered.append(point); now=point
    # Fixed start, open route: 2-opt only reverses intervals after first point.
    for _ in range(20):
        changed=False
        for i in range(1,len(ordered)-1):
            for j in range(i+1,len(ordered)):
                before=math.dist(ordered[i-1],ordered[i])
                after=math.dist(ordered[i-1],ordered[j])
                if j+1<len(ordered):
                    before+=math.dist(ordered[j],ordered[j+1])
                    after+=math.dist(ordered[i],ordered[j+1])
                if after+1e-8<before:
                    ordered[i:j+1]=reversed(ordered[i:j+1]); changed=True
        if not changed: break
    return ordered


def write_job(name,lines):
    path=ROOT/'jobs'/name
    path.write_text('\n'.join(lines)+'\n',encoding='ascii')
    return path


def drill(points,name,tool,diameter,depth,feed,peck):
    lines=header(f'DRILL D{diameter:g} - {len(points)} HOLES',tool)
    for x,y in nearest(points):
        lines += [f'G0 Z{SAFE:.3f}',f'G0 X{x:.4f} Y{y:.4f}','G0 Z0.500']
        level=0
        while level<depth-1e-8:
            level=min(level+peck,depth)
            lines += [f'G1 Z{-level:.4f} F{feed:g}','G0 Z0.500']
    return write_job(name,lines+footer())


def polygons(board,clearance=0):
    if list(board.Zones()): raise ValueError('Zones require a revised polygon exporter')
    items=list(board.GetTracks())+[pad for f in board.GetFootprints() for pad in f.Pads()
                                   if pad.GetNumber() and pad.IsOnLayer(p.B_Cu)]
    result=[]
    for item in items:
        poly=p.SHAPE_POLY_SET()
        item.TransformShapeToPolygon(poly,p.B_Cu,p.FromMM(clearance),p.FromMM(.002),p.ERROR_OUTSIDE)
        for n in range(poly.OutlineCount()):
            if poly.HoleCount(n): raise ValueError('Polygon holes require a revised exporter')
            line=poly.COutline(n)
            result.append([(p.ToMM(line.CPoint(k).x),p.ToMM(line.CPoint(k).y))
                           for k in range(line.PointCount())])
    return result


def phototool(polys,name,negative=True,bottom=False):
    bg,fg=('black','white') if negative else ('white','black')
    parts=[f'<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="64mm" viewBox="-2 -2 40 64">',
           f'<rect x="-2" y="-2" width="40" height="64" fill="{bg}"/>']
    for poly in polys:
        points=' '.join(f'{(WIDTH-x if bottom else x):.5f},{y:.5f}' for x,y in poly)
        parts.append(f'<polygon points="{points}" fill="{fg}"/>')
    parts.append('</svg>')
    (ROOT/'phototools'/name).write_text('\n'.join(parts),encoding='utf-8')
    # Minimal vector PDF: A4, real mm dimensions, no external dependencies.
    k=72/25.4
    cmds=[f'q {k} 0 0 {-k} {50*k} {247*k} cm',
          f'{0 if negative else 1} g -2 -2 40 64 re f',f'{1 if negative else 0} g']
    for poly in polys:
        coords=[(WIDTH-x if bottom else x,y) for x,y in poly]
        cmds.append(' '.join(f'{x:.6f} {y:.6f} '+('m' if i==0 else 'l') for i,(x,y) in enumerate(coords))+' h f')
    cmds += ['Q',f'0 g 0.5 w {50*k} {180*k} m {60*k} {180*k} l S',
             f'BT /F1 9 Tf {50*k} {175*k} Td (10 mm - PRINT ACTUAL SIZE / 100 PERCENT) Tj ET',
             f'BT /F1 9 Tf {50*k} {170*k} Td ({"BOTTOM CONTROL - NOT INK DOWN MASTER" if bottom else "INK SIDE TO RESIST - VERIFY POLARITY"}) Tj ET']
    stream=('\n'.join(cmds)+'\n').encode('ascii')
    objects=[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.27559 841.88976] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>',
        f'<< /Length {len(stream)} >>\nstream\n'.encode()+stream+b'endstream',
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    pdf=b'%PDF-1.4\n'; offsets=[0]
    for i,obj in enumerate(objects,1):
        offsets.append(len(pdf)); pdf+=f'{i} 0 obj\n'.encode()+obj+b'\nendobj\n'
    xref=len(pdf)
    pdf+=b'xref\n0 6\n0000000000 65535 f \n'+b''.join(f'{v:010d} 00000 n \n'.encode() for v in offsets[1:])
    pdf+=f'trailer << /Size 6 /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
    (ROOT/'phototools'/Path(name).with_suffix('.pdf')).write_bytes(pdf)


def intervals(poly,y):
    hits=[]
    for (x1,y1),(x2,y2) in zip(poly,poly[1:]+poly[:1]):
        if (y1<=y<y2) or (y2<=y<y1): hits.append(x1+(y-y1)*(x2-x1)/(y2-y1))
    hits.sort()
    if len(hits)%2: raise ValueError('Odd scanline intersection count')
    return list(zip(hits[::2],hits[1::2]))


def resist(board):
    # Z0 is BARE COPPER. Film thickness is above zero, not part of this depth.
    # Film must already be fully hardened and chemical resistance coupon-tested.
    depth=.005
    diameter=.2+2*depth*math.tan(math.radians(15))
    guard=.01
    keep=polygons(board,diameter/2+guard)
    lines=header('EXPERIMENTAL HARDENED RESIST CLEAR - NOT COPPER MILLING',4)
    lines.insert(1,'(BARE COPPER Z0 - HEIGHT MAP REQUIRED - ZWORK -0.005)')
    length=0.; segments=0
    for row in range(603):
        y=-.1+row*.1
        merged=[]
        spans=sorted(v for poly in keep for v in intervals(poly,y))
        for a,b in spans:
            if merged and a<=merged[-1][1]: merged[-1][1]=max(merged[-1][1],b)
            else: merged.append([a,b])
        clear=[]; end=-.11
        for a,b in merged:
            if a>end: clear.append((end,min(a,WIDTH+.11)))
            end=max(end,b)
        if end<WIDTH+.11: clear.append((end,WIDTH+.11))
        if row%2: clear=[(b,a) for a,b in clear[::-1]]
        for a,b in clear:
            if abs(b-a)<.003: continue
            # Reflect X exactly once; Y converts from PCB down to CNC up.
            x1,x2,yy=WIDTH-a,WIDTH-b,HEIGHT-y
            lines += [f'G0 Z{SAFE:.3f}',f'G0 X{x1:.4f} Y{yy:.4f}',
                      f'G1 Z{-depth:.4f} F30',f'G1 X{x2:.4f} Y{yy:.4f} F150']
            length+=abs(b-a); segments+=1
    write_job('03_EXPERIMENT_resist_clear_UNLEVELLED.ngc',lines+footer())
    return {'tool_effective_width_mm':diameter,'guard_mm':guard,'stepover_mm':.1,
            'depth_relative_to_bare_copper_mm':-depth,'segments':segments,
            'cut_length_mm':round(length,1),'cutting_time_min_without_retracts':round(length/150,1)}


def outline():
    lines=header('OUTLINE REQUIRES 1MM END MILL - NOT V BIT OR DRILL',3)
    points=[(-.5,-.5,False),(16.75,-.5,False),(19.25,-.5,True),(36.5,-.5,False),
            (36.5,28.75,False),(36.5,31.25,True),(36.5,60.5,False),
            (19.25,60.5,False),(16.75,60.5,True),(-.5,60.5,False),
            (-.5,31.25,False),(-.5,28.75,True),(-.5,-.5,False)]
    lines += ['G0 X-0.5000 Y-0.5000']
    level=0.
    while level<2.2-1e-8:
        level=min(level+.2,2.2)
        current=-level
        lines.append(f'G1 Z{current:.4f} F40')
        for x,y,bridge in points[1:]:
            z=max(-level,-1.5) if bridge else -level
            if z!=current: lines.append(f'G1 Z{z:.4f} F40'); current=z
            lines.append(f'G1 X{x:.4f} Y{y:.4f} F120')
    write_job('04_outline_REQUIRES_ENDMILL_1mm.ngc',lines+footer())


def large_holes(holes):
    lines=header('FINAL LARGE HOLES REQUIRE 1MM END MILL AND 2MM PILOTS',3)
    for hole in holes:
        x,y=hole['x_bottom_mm'],hole['y_bottom_mm']
        radius=(hole['nominal_diameter_mm']-1.0)/2
        lines += [f'G0 Z{SAFE:.3f}',f'G0 X{x:.4f} Y{y:.4f}','G0 Z0.500']
        level=0.
        while level<2.2-1e-8:
            level=min(level+.2,2.2)
            lines += [f'G1 Z{-level:.4f} F40',f'G1 X{x+radius:.4f} Y{y:.4f} F100',
                      f'G2 I{-radius:.4f} J0 F100',f'G1 X{x:.4f} Y{y:.4f} F100']
    write_job('05_largeholes_REQUIRES_ENDMILL_1mm.ngc',lines+footer())


def main():
    for name in ['jobs','inputs','phototools','dryrun']: (ROOT/name).mkdir(parents=True,exist_ok=True)
    source=HW/'WaterPressureSensor.kicad_pcb'
    board=p.LoadBoard(str(source))
    edges=[v for item in board.GetDrawings() if item.GetLayer()==p.Edge_Cuts
           for v in [item.GetStart(),item.GetEnd()]]
    if (min(v.x for v in edges),min(v.y for v in edges),max(v.x for v in edges),max(v.y for v in edges))!=(0,0,p.FromMM(WIDTH),p.FromMM(HEIGHT)):
        raise ValueError('Board changed; re-evaluate origin and toolpaths')
    all_holes=[]
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            d=p.ToMM(pad.GetDrillSize().x)
            if not d: continue
            if pad.GetDrillSize().x!=pad.GetDrillSize().y: raise ValueError('Slot needs CAM routing')
            x,y=p.ToMM(pad.GetPosition().x),p.ToMM(pad.GetPosition().y)
            all_holes.append({'ref':fp.GetReference(),'pin':pad.GetNumber(),
                              'nominal_diameter_mm':d,'x_bottom_mm':WIDTH-x,'y_bottom_mm':HEIGHT-y})
    with (ROOT/'holes-bottom.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=list(all_holes[0])); writer.writeheader(); writer.writerows(all_holes)
    small=[(v['x_bottom_mm'],v['y_bottom_mm']) for v in all_holes if v['nominal_diameter_mm']<=1.0]
    big=[(v['x_bottom_mm'],v['y_bottom_mm']) for v in all_holes if v['nominal_diameter_mm']>1.0]
    assert len(small)==24 and len(big)==6
    for name,points,diameter in [('contacts_D1mm.drl',small,1.),('pilots_D2mm.drl',big,2.)]:
        data=['M48','; FULL HEADER / MM / DECIMAL / ABSOLUTE / NOT MIRRORED',
              'METRIC',f'T1C{diameter:.3f}','%','G90','G05','T1']
        data += [f'X{WIDTH-x:.4f}Y{y:.4f}' for x,y in points]
        (ROOT/'inputs'/name).write_text('\n'.join(data+['M30'])+'\n',encoding='ascii')
    drill(small,'01_contacts_D1mm_SETUP_REQUIRED.ngc',1,1.,2.5,60,.35)
    drill(big,'02_PILOTS_ONLY_D2mm_SETUP_REQUIRED.ngc',2,2.,2.85,40,.3)
    poly=polygons(board)
    phototool(poly,'negative_PRINT_INK_DOWN.svg')
    phototool(poly,'negative_BOTTOM_VIEW_CONTROL.svg',bottom=True)
    phototool(poly,'positive_PRINT_INK_DOWN.svg',negative=False)
    experiment=resist(board)
    outline()
    large_holes([v for v in all_holes if v['nominal_diameter_mm']>1])
    for path in (ROOT/'jobs').glob('*.ngc'):
        air=['(AIR RUN - SPINDLE OFF - ALL Z AT PLUS 5 MM)',
             '(VERIFY G54 AND CLAMP CLEARANCE - DISABLE HEIGHT MAP)']
        for line in path.read_text().splitlines():
            if line.startswith(('M3 ','G4 ')): continue
            if not line.startswith('('): line=re.sub(r'Z[+-]?[0-9.]+','Z5.000',line)
            air.append(line)
        (ROOT/'dryrun'/(path.stem+'_AIR5.ngc')).write_text('\n'.join(air)+'\n',encoding='ascii')
    board.GetDesignSettings().SetAuxOrigin(p.VECTOR2I(0,p.FromMM(HEIGHT)))
    board.GetDesignSettings().SetBoardThickness(p.FromMM(2.0))
    p.SaveBoard(str(ROOT/'inputs/WaterPressureSensor-CAM.kicad_pcb'),board)
    manifest={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
              'confirmed_material':{'substrate_mm':2.0,'copper_um':70,'sides':1,'etchant':'FeCl3'},
              'status':'SETUP_AND_COUPON_REQUIRED_NOT_MACHINE_VALIDATED',
              'coordinate_system':'G54, bottom copper facing up; X=36-PCB_X, Y=60-PCB_Y; no further mirror',
              'origin_for_raw_exports':'Drill/place origin in CAM copy at PCB (0,60); raw CAM inputs NOT mirrored',
              'holes':{'contacts_actual_D1mm':24,'of_these_enlarged_from_D0_9mm':4,
                       'D2mm_pilots_only_for_5xD3_2_plus_1xD3_25':6},
              'trial_settings':{'S_command':10000,'clearance_mm':SAFE,
                 'contact_drill_depth_mm':-2.5,'pilot_drill_depth_mm':-2.85,
                 'spindle_calibration':'UNKNOWN: read $30/$31/$32 and verify actual rpm',
                 'drill_point_angle_assumed_degrees':118,'total_blank_thickness_assumed_at_most_mm':2.1},
              'resist_experiment':experiment,
              'outline':'Requires a 1 mm end mill NOT in current tool inventory; tabs approx 1.5 mm wide, 0.5+ mm residual',
              'warning':'No programs have been run on a machine; nonconductive-film probing and etch resistance unresolved'}
    (ROOT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print('Generated setup-required jobs, phototools and CAM input copy. Source PCB unchanged.')
    print(json.dumps(experiment,indent=2))


if __name__=='__main__': main()
