"""Verify generated nominal jobs, origin/scale, resist geometry, and package."""
import hashlib
import json
import math
import re
import zipfile
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import pcbnew as p
from check_gcode import check
from prepare_jobs import polygons,intervals

ROOT=Path(__file__).resolve().parent
manifest=json.loads((ROOT/'manifest.json').read_text())
source=ROOT.parent/'WaterPressureSensor.kicad_pcb'
assert hashlib.sha256(source.read_bytes()).hexdigest()==manifest['source_sha256']
b=p.LoadBoard(str(source))
reports={}
radii={'01':.5,'02':1.,'03':manifest['resist_experiment']['tool_effective_width_mm']/2,'04':.5,'05':.5}
for folder in ['jobs','dryrun']:
    for path in (ROOT/folder).glob('*.ngc'):
        reports[f'{folder}/{path.name}']=check(path.read_text(),[(-2,38),(-2,62),(-3,8)],(0,0,5),radii[path.name[:2]])
        if folder=='dryrun':
            report=reports[f'{folder}/{path.name}']
            assert report['bounds_center_mm']['Z']==(5,5)
            assert not re.search(r'^M3(?:\s|$)',path.read_text(),re.M)

def paths(path):
    position=[0.,0.]
    for line in path.read_text().splitlines():
        if line.startswith(('G0 X','G1 X')):
            words=dict((a,float(v)) for a,v in re.findall(r'([XY])([+-]?[0-9.]+)',line))
            target=[words.get('X',position[0]),words.get('Y',position[1])]
            yield line[:2],position.copy(),target.copy()
            position=target

resist=ROOT/'jobs/03_EXPERIMENT_resist_clear_UNLEVELLED.ngc'
expanded=polygons(b,radii['03'])
segments=[]
for mode,a,c in paths(resist):
    if mode!='G1': continue
    assert abs(a[1]-c[1])<1e-7
    y=60-c[1]; lo,hi=sorted([36-a[0],36-c[0]])
    for poly in expanded:
        for start,end in intervals(poly,y):
            assert min(hi,end)-max(lo,start)<1e-5, 'Resist cutter overlaps retained copper'
    segments.append((a,c))
assert len(segments)==manifest['resist_experiment']['segments']
# Check exact input format/origin signs and matching known connector contact.
gerber=(ROOT/'inputs/WaterPressureSensor-CAM-B_Cu.gbl').read_text()
assert '%FSLAX46Y46*%' in gerber and '%MOMM*%' in gerber
drill=(ROOT/'inputs/WaterPressureSensor-CAM.drl').read_text()
assert 'METRIC' in drill and 'X18.0Y45.0' in drill
assert 'T1C0.900' in drill and 'T4C3.250' in drill

image=Image.new('RGB',(1120,1050),'#f5f7fa'); d=ImageDraw.Draw(image)
font=ImageFont.truetype(r'C:\Windows\Fonts\segoeui.ttf',21)
small=ImageFont.truetype(r'C:\Windows\Fonts\segoeui.ttf',17)
d.text((50,30),'A3 / BOTTOM COPPER / 36 x 60 mm',fill='#102439',font=font)
d.text((50,75),'Drilling + outline',fill='#102439',font=font)
d.text((600,75),'Experimental resist clearing',fill='#102439',font=font)
scale=12; top=140
def xy(point,left): return (left+point[0]*scale,top+(60-point[1])*scale)
for left in [50,600]:
    d.rectangle((left,top,left+36*scale,top+60*scale),fill='white',outline='#64748b',width=2)
    for poly in polygons(b):
        d.polygon([xy((36-x,60-y),left) for x,y in poly],fill='#158060')
for a,c in segments: d.line([xy(a,600),xy(c,600)],fill='#d69a55',width=1)
outline=ROOT/'jobs/04_outline_REQUIRES_ENDMILL_1mm.ngc'
for mode,a,c in paths(outline):
    if mode=='G1': d.line([xy(a,50),xy(c,50)],fill='#3d66cf',width=2)
for prefix,radius in [('01',.5),('02',1.)]:
    path=next((ROOT/'jobs').glob(prefix+'*.ngc'))
    for mode,a,c in paths(path):
        if mode!='G0': continue
        x,y=xy(c,50); r=radius*scale
        d.ellipse((x-r,y-r,x+r,y+r),fill='white',outline='#172637',width=1)
d.text((50,900),'24 contacts D1 + 6 pilots D2',fill='#102439',font=small)
d.text((50,930),'Blue: outline; needs 1 mm end mill',fill='#3d66cf',font=small)
d.text((600,900),'Green: retained copper',fill='#158060',font=small)
d.text((600,930),'Orange: nominal film tool path',fill='#9c672f',font=small)
d.text((50,995),'Preview only. Jobs are mirrored once. Raw CAM inputs are NOT mirrored.',fill='#102439',font=small)
image.save(ROOT/'CAM-preview.png')
report={'source_unchanged':True,'input_format':'mm / Gerber 4.6 / Excellon decimal',
        'nominal_resist_sweep_preserves_copper':True,'resist_segments_checked':len(segments),
        'checks':reports,'machine_validated':False,
        'remaining':['Verify actual GRBL settings, work offsets and clearance',
                     'Coupon trials and real thickness/point-angle/rpm checks',
                     'No valid height map has been supplied',
                     'Dry film brand/polarity/exposure/etch resistance not validated',
                     '1 mm end mill is required for final large holes and contour']}
(ROOT/'static-checks.json').write_text(json.dumps(report,indent=2)+'\n')
archive=ROOT.parent/'exports/WaterPressureSensor-A3-CNC-review.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for file in ROOT.rglob('*'):
        if file.is_file() and '__pycache__' not in file.parts and file.suffix!='.pyc':
            z.write(file,Path('A3-CNC')/file.relative_to(ROOT))
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    print('Archive CRC OK:',len(z.namelist()),'files')
print('10 nominal/air jobs passed static checks. All air-run Z=+5, spindle off.')
print('Nominal resist sweeps avoid retained copper. Actual machine/height map unverified.')
print(archive)
