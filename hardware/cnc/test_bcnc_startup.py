"""Offline bCNC startup smoke check. Run with the dedicated bCNC venv.

Uses a separate test ini, blocks serial-port opens, and closes its hidden window.
Does not send commands to a machine or change the user's regular bCNC profile.
"""
import importlib.metadata
import json
import os
from pathlib import Path
import sys
import tkinter as tk

import cv2
import numpy as np
import serial

runtime=Path(os.environ['LOCALAPPDATA'])/'bCNC'
(runtime/'startup-test.ini').write_text('[Connection]\npendant=0\nopenserial=0\ncontroller=GRBL1\n',encoding='utf-8')
report={'python':sys.version.split()[0],
        'bCNC':importlib.metadata.version('bCNC'),
        'numpy':np.__version__,'opencv':cv2.__version__}
image=cv2.cvtColor(np.zeros((4,4,3),dtype=np.uint8),cv2.COLOR_BGR2GRAY)
assert image.shape==(4,4)
report['opencv_numpy_operation']='PASS'

interp=tk.Tcl()
report['tcl']=interp.call('info','patchlevel')
variable=tk.StringVar(interp)
callbacks=[]
variable.trace('w',lambda *args:callbacks.append(True))
variable.set('test')
assert callbacks
report['legacy_tk_trace']='PASS'

class BlockedSerial(serial.Serial):
    def open(self): raise RuntimeError('Serial open blocked during offline test')
serial.Serial=BlockedSerial
def blocked_port(*args,**kwargs):
    raise RuntimeError('Serial connection blocked during offline test')
serial.serial_for_url=blocked_port

import bCNC.__main__ as entry
import Helpers
import Utils
Utils.iniUser=str(runtime/'startup-test.ini')
Utils.loadConfiguration(systemOnly=True)
Utils._errorReport=False
import bmain
import Updates
Updates.need2Check=lambda:False

# Hide the test root before constructing all the application widgets.
tk_init=tk.Tk.__init__
def hidden_init(self,*args,**kwargs):
    tk_init(self,*args,**kwargs)
    self.withdraw()
tk.Tk.__init__=hidden_init

original_init=bmain.Application.__init__
def test_init(self,*args,**kwargs):
    original_init(self,*args,**kwargs)
    report['tk']=self.tk.call('package','require','Tk')
    report['application_widgets']='PASS'
    assert self.serial is None
    sample=next((Path(__file__).resolve().parent/'dryrun').glob('01*_AIR5.ngc'))
    self.load(str(sample))
    report['loaded_air_job']=sample.name
    report['loaded_blocks']=len(self.gcode.blocks)
    self.update_idletasks()
    self.after(500,self.quit)
bmain.Application.__init__=test_init
sys.argv=['bCNC','--ini',str(runtime/'startup-test.ini')]
entry.main()

from CNC import Probe
probe=Probe()
probe.xmax=probe.ymax=1.; probe.xn=probe.yn=2
probe.xstep(); probe.ystep(); probe.makeMatrix(); probe.start=True
for x,y,z in [(0,0,0),(1,0,.1),(0,1,.2),(1,1,.3)]: probe.add(x,y,z)
assert abs(probe.interpolate(.5,.5)-.15)<1e-9
report['synthetic_probe_interpolation']='PASS'
report['hardware_connected']=False
report['status']='PASS_OFFLINE_STARTUP'
(runtime/'startup-check.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
