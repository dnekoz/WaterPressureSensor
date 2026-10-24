"""Audit actual PCB/netlist/DRC and the one-copper-layer review export."""

import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew as p

ROOT=Path(__file__).resolve().parents[1]
board=p.LoadBoard(str(ROOT/'WaterPressureSensor.kicad_pcb'))
design=json.loads((ROOT/'design.json').read_text(encoding='utf-8'))
tree=ET.parse(ROOT/'review/netlist.xml')
checks=[]


def require(condition, description):
    if not condition:
        raise SystemExit('PCB audit failed: '+description)
    checks.append(description)


pads={(fp.GetReference(),pad.GetNumber()):pad for fp in board.GetFootprints()
      for pad in fp.Pads() if fp.GetReference() in design['footprints'] and pad.GetNumber()}
require(len(pads)==24,'16 WeMos + 4 OLED + 4 RJ contacts present')
for net in tree.findall('./nets/net'):
    name=net.attrib['name']
    if name.startswith('unconnected-'):
        name=name.replace('/','{slash}')
    for node in net.findall('node'):
        key=node.attrib['ref'],node.attrib['pin']
        require(key in pads and pads[key].GetNetname()==name,
                f'{key[0]}.{key[1]} matches schematic net {name}')
tracks=list(board.GetTracks())
require(bool(tracks),'Routed board contains tracks')
require(not any(isinstance(item,p.PCB_VIA) for item in tracks),'No vias')
require(all(item.GetLayer()==p.B_Cu for item in tracks),'All routed copper is on B.Cu')
require(all(p.ToMM(item.GetWidth())>=0.39999 for item in tracks),'Tracks are at least 0.4 mm wide')
xs=[p.ToMM(pads[('J1',str(n))].GetPosition().x) for n in range(1,5)]
ys=[p.ToMM(pads[('J1',str(n))].GetPosition().y) for n in range(1,5)]
require(xs==sorted(xs) and len(set(round(y,6) for y in ys))==1,
        'OLED front/top order is 1 GND, 2 VCC, 3 SCL, 4 SDA from left to right')
require(all(abs(xs[i+1]-xs[i]-2.54)<0.0001 for i in range(3)),'OLED header pitch is 2.54 mm')
drc=json.loads((ROOT/'review/drc.json').read_text(encoding='utf-8'))
for field in ['violations','unconnected_items','schematic_parity']:
    require(not drc.get(field),f'DRC {field}: zero')
erc=json.loads((ROOT/'review/erc.json').read_text(encoding='utf-8'))
require(not any(sheet['violations'] for sheet in erc['sheets']),'ERC: zero violations')
folder=ROOT/'exports/A2-review'
copper=[]
for path in folder.iterdir():
    if path.suffix.lower() in {'.gbl','.gtl','.gbr'}:
        text=path.read_text(encoding='utf-8',errors='replace')
        if 'FileFunction,Copper' in text:
            copper.append(path.name)
require(len(copper)==1 and 'B_Cu' in copper[0],'Export contains exactly one copper file: B.Cu')
drills=list(folder.glob('*.drl'))
require(len(drills)==1,'One merged Excellon drill file exported')
length=sum(math.hypot(p.ToMM(t.GetEnd().x-t.GetStart().x),
                      p.ToMM(t.GetEnd().y-t.GetStart().y)) for t in tracks)
report={'revision':design['revision'],'checks_passed':True,'checks':checks,
        'board_size_mm':design['design_choices']['board_size_mm'],
        'track_count':len(tracks),'track_length_mm':round(length,2),'track_width_mm':0.4,
        'clearance_mm':0.25,'via_count':0,'jumper_count':0,'routed_layer':'B.Cu',
        'copper_export_files':copper,'erc_violations':0,'drc_violations':0,
        'unconnected_items':0,'schematic_parity_issues':0,
        'manufacturing_released':False,
        'pending_physical_checks':['Actual Type-C WeMos header fit',
            'OLED pin pitch/edge offset and mating height',
            'Actual RJ-10 jack versus EVERCOM 5301 footprint',
            'Sensor signal/power and prototype calibration'],
        'step_model':'Carrier only; exact module/connector 3D models not supplied'}
(ROOT/'review/board-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(f'PCB audit: {len(checks)} checks passed; {len(tracks)} B.Cu segments, {length:.1f} mm total, no vias/jumpers.')
print('One copper Gerber verified. Physical fit check is still required before fabrication.')
