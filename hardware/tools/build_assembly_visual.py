"""Build approximate, local 3D assembly models on a copy of the actual PCB.

Run with KiCad Python. The source PCB is read only. These are visualization
envelopes, not manufacturer STEP files or a mechanical clearance validation.
VRML model coordinates use KiCad's 0.1-inch unit (2.54 mm).
"""

import hashlib
import json
import math
from pathlib import Path

import pcbnew as p

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'visualization'
MODELS = OUT / 'models'
UNIT = 2.54
COLORS = {
    'blue': (0.018, .18, .48), 'darkblue': (.012, .055, .13),
    'black': (.018, .021, .026), 'plastic': (.035, .039, .045),
    'glass': (.008, .018, .025), 'metal': (.66, .69, .72),
    'gold': (.77, .58, .18), 'silk': (.90, .93, .95),
    'cyan': (.04, .78, .96), 'ceramic': (.53, .38, .23),
}
FONT = {
 '0':['01110','10001','10011','10101','11001','10001','01110'],
 '1':['00100','01100','00100','00100','00100','00100','01110'],
 '2':['01110','10001','00001','00010','00100','01000','11111'],
 '3':['11110','00001','00001','01110','00001','00001','11110'],
 '4':['00010','00110','01010','10010','11111','00010','00010'],
 '5':['11111','10000','10000','11110','00001','00001','11110'],
 '6':['01110','10000','10000','11110','10001','10001','01110'],
 '7':['11111','00001','00010','00100','01000','01000','01000'],
 '8':['01110','10001','10001','01110','10001','10001','01110'],
 '9':['01110','10001','10001','01111','00001','00001','01110'],
 'A':['01110','10001','10001','11111','10001','10001','10001'],
 'B':['11110','10001','10001','11110','10001','10001','11110'],
 'C':['01111','10000','10000','10000','10000','10000','01111'],
 'D':['11110','10001','10001','10001','10001','10001','11110'],
 'E':['11111','10000','10000','11110','10000','10000','11111'],
 'F':['11111','10000','10000','11110','10000','10000','10000'],
 'G':['01111','10000','10000','10111','10001','10001','01111'],
 'H':['10001','10001','10001','11111','10001','10001','10001'],
 'I':['11111','00100','00100','00100','00100','00100','11111'],
 'L':['10000','10000','10000','10000','10000','10000','11111'],
 'M':['10001','11011','10101','10101','10001','10001','10001'],
 'N':['10001','11001','10101','10011','10001','10001','10001'],
 'O':['01110','10001','10001','10001','10001','10001','01110'],
 'P':['11110','10001','10001','11110','10000','10000','10000'],
 'R':['11110','10001','10001','11110','10100','10010','10001'],
 'S':['01111','10000','10000','01110','00001','00001','11110'],
 'T':['11111','00100','00100','00100','00100','00100','00100'],
 'U':['10001','10001','10001','10001','10001','10001','01110'],
 'V':['10001','10001','10001','10001','10001','01010','00100'],
 'W':['10001','10001','10001','10101','10101','10101','01010'],
 'Y':['10001','10001','01010','00100','00100','00100','00100'],
 '.':['00000','00000','00000','00000','00000','00110','00110'],
 '-':['00000','00000','00000','11111','00000','00000','00000'],
 ' ':['00000']*7,
}


class Model:
    """Input coordinates are footprint-local PCB x/y, with z above the carrier."""
    def __init__(self):
        self.nodes = []

    def material(self, color):
        rgb = COLORS[color]
        emissive = tuple(c * .55 for c in rgb) if color == 'cyan' else (0,0,0)
        return ('appearance Appearance { material Material { '
                f'diffuseColor {rgb[0]} {rgb[1]} {rgb[2]} '
                f'emissiveColor {emissive[0]} {emissive[1]} {emissive[2]} '
                'specularColor 0.35 0.35 0.35 shininess 0.35 } }')

    def box(self, x1,y1,z1,x2,y2,z2,color='black'):
        assert x2>x1 and y2>y1 and z2>z1
        cx,cy,cz=(x1+x2)/2/UNIT,-(y1+y2)/2/UNIT,(z1+z2)/2/UNIT
        dx,dy,dz=(x2-x1)/UNIT,(y2-y1)/UNIT,(z2-z1)/UNIT
        self.nodes.append(f'Transform {{ translation {cx} {cy} {cz} children [ '
            f'Shape {{ {self.material(color)} geometry Box {{ size {dx} {dy} {dz} }} }} ] }}')

    def cylinder(self,x,y,z1,z2,radius,color='metal'):
        self.nodes.append(f'Transform {{ translation {x/UNIT} {-y/UNIT} {(z1+z2)/2/UNIT} '
            f'rotation 1 0 0 {math.pi/2} children [ Shape {{ {self.material(color)} '
            f'geometry Cylinder {{ radius {radius/UNIT} height {(z2-z1)/UNIT} }} }} ] }}')

    def ring(self,x,y,z,outer,inner,color='gold'):
        points=[]
        for rad in [outer,inner]:
            for n in range(32):
                angle=2*math.pi*n/32
                points.append(f'{(x+rad*math.cos(angle))/UNIT} {-(y+rad*math.sin(angle))/UNIT} {z/UNIT}')
        indices=[' '.join(str(v) for v in [n,(n+1)%32,32+(n+1)%32,32+n,-1]) for n in range(32)]
        self.nodes.append(f'Shape {{ {self.material(color)} geometry IndexedFaceSet {{ solid FALSE '
            f'coord Coordinate {{ point [ {", ".join(points)} ] }} coordIndex [ {", ".join(indices)} ] }} }}')

    def text(self,text,x,y,z,pixel=.16,color='silk',transform=None):
        for index,ch in enumerate(text.upper()):
            for yy,row in enumerate(FONT[ch]):
                for xx,on in enumerate(row):
                    if on!='1': continue
                    a,b=x+(index*6+xx)*pixel,y+yy*pixel
                    if transform:
                        corners=[transform(a,b),transform(a+pixel*.88,b+pixel*.88)]
                        x1,x2=sorted(v[0] for v in corners)
                        y1,y2=sorted(v[1] for v in corners)
                    else:
                        x1,y1,x2,y2=a,b,a+pixel*.88,b+pixel*.88
                    self.box(x1,y1,z,x2,y2,z+.012,color)

    def save(self,name):
        path=MODELS/(name+'.wrl')
        path.write_text('#VRML V2.0 utf8\n# Approximate assembly visualization; dimensions in 0.1 inch.\n'
                        +'\n'.join(self.nodes)+'\n',encoding='utf-8')
        return path


def wemos():
    m=Model()
    # Two female carrier sockets, assumed 8.5 mm body, with male headers above.
    for x in (0,22.86):
        m.box(x-1.27,-1.27,0,x+1.27,19.05,8.5,'plastic')
        m.box(x-1.25,-1.25,8.5,x+1.25,19.03,11.0,'black')
        for n in range(8):
            y=2.54*n
            m.box(x-.32,y-.32,-2.2,x+.32,y+.32,12.5,'metal')
    m.box(-1.37,-8.21,11,24.23,26.11,12,'blue')
    for x in (0,22.86):
        for n in range(8):
            m.ring(x,n*2.54,12.025,.88,.38)
    # Representative ESP8266 shield and PCB antenna; not an exact module revision.
    m.box(3.6,-1.1,12.02,19.0,13.6,12.6,'darkblue')
    m.box(4.0,-.6,12.6,18.6,13.1,15.1,'metal')
    m.text('ESP8266',5.2,4.0,15.13,.19,'plastic')
    for i in range(6):
        x=4.3+i*2.3
        m.box(x,-6.8,12.02,x+.42,-2.1,12.07,'gold')
        if i<5:
            y=-6.8 if i%2==0 else -2.52
            m.box(x,y,12.02,x+2.72,y+.42,12.07,'gold')
    # USB Type-C shell at the outward edge; opening made with separate walls.
    x1,x2,y1,y2=7.0,15.6,19.4,26.35
    m.box(x1,y1,12.1,x2,y2,12.4,'metal')
    m.box(x1,y1,14.95,x2,y2,15.25,'metal')
    m.box(x1,y1,12.4,x1+.35,y2,14.95,'metal')
    m.box(x2-.35,y1,12.4,x2,y2,14.95,'metal')
    m.box(x1+.35,y1,12.4,x2-.35,y1+.6,14.95,'plastic')
    m.box(x1+1.0,y1,13.25,x2-1,y2-.25,13.85,'plastic')
    for i in range(10):
        x=x1+1.3+i*.59
        m.box(x,y2-1.2,13.87,x+.24,y2-.4,13.9,'gold')
    m.box(3.8,18.3,12,6.5,22.6,13.1,'black')
    m.box(18.2,18,12,21,22,13.3,'black')
    for x,y in [(2.1,15.5),(5,15.2),(16.2,15.4),(19,16),(16.3,23.5)]:
        m.box(x,y,12,x+1.1,y+1.8,12.7,'ceramic')
        m.box(x,y,12.01,x+1.1,y+.24,12.72,'metal')
        m.box(x,y+1.55,12.01,x+1.1,y+1.8,12.72,'metal')
    m.text('D1 MINI',2.4,24.0,12.04,.17)
    return m.save('wemos_typec_approx')


def oled():
    m=Model()
    m.box(-1.205,-1.525,0,1.205,9.145,18.67,'plastic')
    for n in range(4):
        y=n*2.54
        m.box(-.3,y-.3,-2.2,.3,y+.3,21.5,'metal')
        m.ring(0,y,18.69,.57,.36,'gold')
    m.box(-1.25,-1.3,18.67,1.25,8.92,21.17,'black')
    m.box(-25,-9.69,21.17,2,17.31,22.37,'blue')
    for n in range(4): m.ring(0,n*2.54,22.4,.83,.37)
    # Screen geometry defined in board-aligned coordinates relative to J1.
    # PCB rotation +90 maps local (x,y) to board (y,-x).
    def rect(a,b,z1,c,d,z2,color):
        m.box(-d,a,z1,-b,c,z2,color)
    rect(-8.6,2.9,22.4,16.2,22.9,23.45,'black')
    rect(-8.1,3.4,23.45,15.7,22.3,23.7,'glass')
    # Active 0.96-inch area 21.744 x 10.872 mm, 128 x 64 reference pixels.
    sx,sy=-7.04,6.0
    rect(sx,sy,23.71,sx+21.744,sy+10.872,23.73,'glass')
    transform=lambda a,b:(-b,a)
    m.text('PRESSURE',sx+.68,sy+.68,23.75,.1699,'cyan',transform)
    m.text('2.50 BAR',sx+2.0,sy+4.1,23.75,.3398,'cyan',transform)
    m.text('DEMO',sx+.68,sy+9.0,23.75,.1699,'cyan',transform)
    # Flat flexible tail at lower glass edge.
    rect(-3.5,23.0,22.38,11.4,24.6,22.5,'gold')
    for n,label in enumerate(['GND','VCC','SCL','SDA']):
        m.text(label,n*2.54-1.0,-1.1,22.42,.105,'silk',transform)
    return m.save('oled_ssd1306_tall_socket_approx')


def rj10():
    m=Model()
    m.box(-3.71,-3.75,0,7.39,6.5,11.4,'plastic')
    m.box(-3.71,6.5,0,7.39,14.25,1.35,'plastic')
    m.box(-3.71,6.5,8.85,7.39,14.25,11.4,'plastic')
    m.box(-3.71,6.5,1.35,-2.0,14.25,8.85,'plastic')
    m.box(5.68,6.5,1.35,7.39,14.25,8.85,'plastic')
    for i in range(4):
        x=.31+i*1.02
        m.box(x,6.2,7.7,x+.4,12.65,8.0,'gold')
    for x,y in [(0,0),(1.27,-2.54),(2.54,0),(3.81,-2.54)]:
        m.box(x-.25,y-.25,-2.2,x+.25,y+.25,.15,'metal')
    for x in [-1.91,5.59]: m.cylinder(x,6.35,-2.2,.2,1.5,'black')
    m.text('4P4C',-.4,.6,11.42,.20,'silk')
    return m.save('rj10_4p4c_approx')


def main():
    MODELS.mkdir(parents=True,exist_ok=True)
    paths={'U1':wemos(),'J1':oled(),'J2':rj10()}
    source=ROOT/'WaterPressureSensor.kicad_pcb'
    board=p.LoadBoard(str(source))
    for fp in board.GetFootprints():
        if fp.GetReference() in paths:
            fp.Models().clear()
            model=p.FP_3DMODEL()
            model.m_Filename='${KIPRJMOD}/models/'+paths[fp.GetReference()].name
            model.m_Scale=p.VECTOR3D(1,1,1)
            model.m_Offset=p.VECTOR3D(0,0,0)
            model.m_Rotation=p.VECTOR3D(0,0,0)
            fp.Add3DModel(model)
    title=board.GetTitleBlock()
    title.SetTitle('WaterPressureSensor A3 - illustrative assembly, approximate module models')
    target=OUT/'WaterPressureSensor-assembled.kicad_pcb'
    p.SaveBoard(str(target),board)
    metadata={
        'source_pcb':str(source.relative_to(ROOT)),
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'assembly_pcb':target.name,
        'models':'Locally generated approximate VRML; no manufacturer 3D models',
        'model_heights_mm_above_carrier_top':{
            'wemos_female_socket_top':8.5,'wemos_pcb_bottom':11,
            'wemos_pcb_top':12,'wemos_usb_top':15.25,
            'oled_socket_top':18.67,'oled_pcb_bottom':21.17,
            'oled_pcb_top':22.37,'oled_glass_top':23.75,'rj10_top':11.4},
        'display_value':'2.50 BAR / DEMO is an illustration, not a sensor reading',
        'limitations':['Exact module revision, underside parts and mated height not verified',
                       'USB-C and RJ10 shells and contact geometry are illustrative',
                       'No mounting screws, enclosure, cable or OLED support are implied',
                       'This visualization is not a mechanical fit or clearance certification']}
    (OUT/'assembly-assumptions.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
    print('Saved assembly visualization:',target)
    print('Original PCB preserved:',metadata['source_sha256'])


if __name__=='__main__': main()
