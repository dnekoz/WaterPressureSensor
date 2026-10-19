"""Build and route the single-copper-side carrier for mechanical/electrical review.

Run with KiCad's Python. The exported XML netlist is the connectivity authority.
The routing grid does not replace KiCad DRC; run DRC including schematic parity.
"""

import argparse
import heapq
import itertools
import json
import math
import random
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew as p
from PIL import Image, ImageDraw

from build_schematic import uid

ROOT = Path(__file__).resolve().parents[1]
STD = Path(r"C:\Program Files\KiCad\10.0\share\kicad\footprints")
LOCAL = ROOT / "WaterPressureSensor.pretty"
PLUGIN = p.PCB_IO_MGR.FindPlugin(p.PCB_IO_MGR.KICAD_SEXP)
STEP = 0.127
WIDTH, HEIGHT = 46.0, 65.0
TRACE, CLEARANCE = 0.4, 0.25
NX, NY = math.ceil(WIDTH / STEP) + 1, math.ceil(HEIGHT / STEP) + 1
MOVES = [(1, 0), (0, 1), (-1, 0), (0, -1), (1, 1), (-1, 1), (-1, -1), (1, -1)]


def pt(x, y):
    return p.VECTOR2I(p.FromMM(x), p.FromMM(y))


def mm(v):
    return p.ToMM(v)


def through_pad_layers():
    # KiCad normalizes through-hole pads to *.Cu in its two-layer minimum
    # stack. Manufacturing exports include ONLY B.Cu; no F.Cu file is issued.
    result = p.LSET.AllCuMask()
    result.AddLayer(p.B_Mask)
    return result


def line(parent, a, b, layer, width=0.15):
    shape = p.PCB_SHAPE(parent)
    shape.SetShape(p.SHAPE_T_SEGMENT)
    shape.SetStart(pt(*a))
    shape.SetEnd(pt(*b))
    shape.SetLayer(layer)
    shape.SetWidth(p.FromMM(width))
    parent.Add(shape)
    return shape


def rectangle(parent, x1, y1, x2, y2, layer, width=0.15):
    for a, b in [((x1,y1),(x2,y1)), ((x2,y1),(x2,y2)),
                 ((x2,y2),(x1,y2)), ((x1,y2),(x1,y1))]:
        line(parent, a, b, layer, width)


def label(board, value, x, y, layer=p.F_SilkS, size=1.0):
    item = p.PCB_TEXT(board)
    item.SetText(value)
    item.SetPosition(pt(x, y))
    item.SetLayer(layer)
    item.SetTextSize(pt(size, size))
    item.SetTextThickness(p.FromMM(0.15))
    board.Add(item)


def stock(library, name, local_name):
    fp = p.FootprintLoad(str(STD / (library + ".pretty")), name)
    if fp is None:
        raise RuntimeError("Footprint unavailable: " + name)
    fp.SetFPID(p.LIB_ID("WaterPressureSensor", local_name))
    # Models for the legacy micro-USB module/ordinary-height header would be
    # misleading for the selected Type-C/elevated assembly. Do not substitute them.
    fp.Models().clear()
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    if library == "RF_Module":
        # Retain the complete USB outline on F.Fab; keep its insertion end
        # clear on the silkscreen.
        for shape in list(fp.GraphicalItems()):
            if shape.GetLayer() == p.F_SilkS and isinstance(shape, p.PCB_SHAPE):
                if max(mm(shape.GetStart().y), mm(shape.GetEnd().y)) > 25.5:
                    fp.Remove(shape)
    for pad in fp.Pads():
        if pad.GetAttribute() == p.PAD_ATTRIB_PTH:
            pad.SetLayerSet(through_pad_layers())
            if library == "RF_Module":
                pad.SetDrillSize(pt(1.0, 1.0))
    PLUGIN.FootprintSave(str(LOCAL), fp)
    return fp


def oled_socket():
    name = "OLED_ESW_1x04_P2.54_H18.67"
    fp = p.FOOTPRINT(None)
    fp.SetFPID(p.LIB_ID("WaterPressureSensor", name))
    fp.SetAttributes(p.FP_THROUGH_HOLE)
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    fp.SetLibDescription("Samtec ESW-104-44-G-S: 2.54 pitch, 18.67 body; mechanical mating check required")
    for i in range(4):
        pad = p.PAD(fp)
        pad.SetNumber(str(i + 1))
        pad.SetAttribute(p.PAD_ATTRIB_PTH)
        pad.SetShape(p.PAD_SHAPE_RECT if i == 0 else p.PAD_SHAPE_CIRCLE)
        pad.SetSize(pt(1.8, 1.8))
        pad.SetDrillSize(pt(1.0, 1.0))
        pad.SetPosition(pt(0, i * 2.54))
        pad.SetLayerSet(through_pad_layers())
        fp.Add(pad)
    rectangle(fp, -1.205, -1.525, 1.205, 9.145, p.F_Fab)
    rectangle(fp, -1.34, -1.66, 1.34, 9.28, p.F_SilkS, 0.12)
    rectangle(fp, -1.8, -2.2, 1.8, 9.82, p.F_CrtYd, 0.05)
    PLUGIN.FootprintSave(str(LOCAL), fp)
    return fp


def prepare_project():
    path = ROOT / "WaterPressureSensor.kicad_pro"
    project = json.loads(path.read_text(encoding="utf-8"))
    project.setdefault("text_variables", {})["HW_REV"] = "A2 LAYOUT REVIEW"
    settings = project.setdefault("board", {}).setdefault("design_settings", {})
    settings["rules"] = {"min_clearance": CLEARANCE, "min_track_width": TRACE,
                         "min_copper_edge_clearance": 0.5, "min_hole_clearance": 0.25,
                         "min_hole_to_hole": 0.25, "min_through_hole_diameter": 0.8,
                         "min_silk_clearance": 0.15, "min_text_height": 0.8,
                         "min_text_thickness": 0.12}
    project["net_settings"] = {"meta": {"version": 4}, "classes": [
        {"name": "Default", "clearance": CLEARANCE, "track_width": TRACE,
         "via_diameter": 1.2, "via_drill": 0.6, "bus_width": 12,
         "wire_width": 6, "diff_pair_width": TRACE, "diff_pair_gap": CLEARANCE,
         "diff_pair_via_gap": CLEARANCE, "microvia_diameter": 0.5,
         "microvia_drill": 0.127, "priority": 2147483647,
         "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)"}
    ], "netclass_patterns": []}
    path.write_text(json.dumps(project, indent=2) + "\n", encoding="utf-8")


def make_board():
    LOCAL.mkdir(exist_ok=True)
    prepare_project()
    (ROOT / "fp-lib-table").write_text(
        '(fp_lib_table (version 7) (lib (name "WaterPressureSensor") (type "KiCad") '
        '(uri "${KIPRJMOD}/WaterPressureSensor.pretty") (options "") (descr "Local single-sided carrier footprints")))\n',
        encoding="utf-8")
    board = p.BOARD()
    board.SetCopperLayerCount(2)  # Format minimum. All routing and the sole copper export use B.Cu.
    board.GetDesignSettings().SetBoardThickness(p.FromMM(1.6))
    board.GetTitleBlock().SetTitle("WaterPressureSensor - A2 layout review")
    board.GetTitleBlock().SetRevision("A2")
    board.GetTitleBlock().SetDate("2026-10-17")
    tree = ET.parse(ROOT / "review/netlist.xml")
    values = {item.attrib["ref"]: item.findtext("value") for item in tree.findall("./components/comp")}
    lookup, nets = {}, {}
    for item in tree.findall("./nets/net"):
        name = item.attrib["name"]
        if name.startswith("unconnected-"):
            name = name.replace("/", "{slash}")
        net = p.NETINFO_ITEM(board, name)
        board.Add(net)
        nets[name] = net
        for node in item.findall("node"):
            lookup[(node.attrib["ref"], node.attrib["pin"])] = net
    parts = {
        "U1": (stock("RF_Module", "WEMOS_D1_mini_light", "WEMOS_D1_mini_TypeC_headers"), (15.875,36.195), 90),
        "J1": (oled_socket(), (26.67,5.715), 90),
        "J2": (stock("Connector_RJ", "RJ9_Evercom_5301-440xxx_Horizontal", "RJ10_Evercom_5301_4P4C"), (22.86,50.8), 0),
    }
    for ref, (fp, position, angle) in parts.items():
        fp.SetReference(ref)
        fp.SetValue(values[ref])
        fp.SetPath(p.KIID_PATH("/" + uid("sheet") + "/" + uid(ref)))
        fp.SetOrientationDegrees(angle)
        fp.SetPosition(pt(*position))
        fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)
        for pad in fp.Pads():
            key = (ref, pad.GetNumber())
            if key in lookup:
                pad.SetNet(lookup[key])
        board.Add(fp)
    for i, (x, y) in enumerate([(3.5,3.5),(42.5,3.5),(3.5,61.5),(42.5,61.5)], 1):
        fp = p.FootprintLoad(str(STD / "MountingHole.pretty"), "MountingHole_3.2mm_M3")
        fp.SetReference(f"H{i}")
        fp.SetAttributes(p.FP_BOARD_ONLY | p.FP_EXCLUDE_FROM_BOM | p.FP_EXCLUDE_FROM_POS_FILES)
        fp.SetPosition(pt(x, y))
        fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)
        board.Add(fp)
    rectangle(board, 0, 0, WIDTH, HEIGHT, p.Edge_Cuts, 0.05)
    # Envelope only: no unverified OLED mounting holes are drilled into the carrier.
    rectangle(board, 16.98, 3.715, 43.98, 30.715, p.Dwgs_User, 0.15)
    label(board, "OLED 27x27 - upper tier", 30.48, 20.0, p.Dwgs_User, 1.0)
    label(board, "J1 OLED", 30, 2.4)
    label(board, "GND  3V3  SCL  SDA", 30.48, 9.0, size=0.8)
    label(board, "U1 D1 MINI", 23, 40.0)
    label(board, "RJ10 / 4P4C", 25.5, 43.5)
    label(board, "1 +5V\n2 GND\n3 NC\n4 A0", 9.0, 49.0, size=0.9)
    label(board, "WPS A2  /  B.Cu only", 19.0, 61.5, size=0.8)
    label(board, "USB-C", 37.0, 39.5, size=0.8)
    line(board, (5,69), (15,69), p.Dwgs_User, 0.15)
    line(board, (5,68.5), (5,69.5), p.Dwgs_User, 0.15)
    line(board, (15,68.5), (15,69.5), p.Dwgs_User, 0.15)
    label(board, "10 mm / print 100%", 30, 69, p.Dwgs_User, 0.9)
    label(board, "46 x 65 mm - A2 fit check", 23, 72, p.Dwgs_User, 1.0)
    return board


def paint_rect(draw, bounds, value=1):
    x1, y1, x2, y2 = bounds
    box = (max(0, math.ceil(x1/STEP)), max(0, math.ceil(y1/STEP)),
           min(NX-1, math.floor(x2/STEP)), min(NY-1, math.floor(y2/STEP)))
    if box[0] <= box[2] and box[1] <= box[3]:
        draw.rectangle(box, fill=value)


def static_mask(board, name):
    mask = Image.new("L", (NX,NY), 0)
    draw = ImageDraw.Draw(mask)
    border = 0.8
    paint_rect(draw, (0,0,WIDTH,border))
    paint_rect(draw, (0,HEIGHT-border,WIDTH,HEIGHT))
    paint_rect(draw, (0,0,border,HEIGHT))
    paint_rect(draw, (WIDTH-border,0,WIDTH,HEIGHT))
    # Read the antenna/routing rule areas in their current board coordinates.
    # This also supports routing an edited placement without recreating it.
    m = TRACE/2 + 0.05
    zones = list(board.Zones())
    zones.extend(z for fp in board.GetFootprints() for z in fp.Zones())
    for zone in zones:
        if zone.GetIsRuleArea() and zone.GetDoNotAllowTracks():
            box = zone.GetBoundingBox()
            paint_rect(draw, (mm(box.GetLeft())-m, mm(box.GetTop())-m,
                              mm(box.GetRight())+m, mm(box.GetBottom())+m))
    margin = CLEARANCE + TRACE/2
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNumber() and pad.GetNetname() == name:
                continue
            box = pad.GetBoundingBox()
            paint_rect(draw, (mm(box.GetLeft())-margin, mm(box.GetTop())-margin,
                              mm(box.GetRight())+margin, mm(box.GetBottom())+margin))
    return mask


def route_path(mask, starts, target):
    pixels = mask.tobytes()
    goal = target[1]*NX+target[0]
    dist, prev, queue = {}, {}, []
    def heuristic(x,y):
        a,b=abs(x-target[0]),abs(y-target[1])
        return max(a,b) + 0.41421356237*min(a,b)
    for x,y in starts:
        idx=y*NX+x
        dist[idx]=0.0
        prev[idx]=None
        heapq.heappush(queue,(heuristic(x,y),0.0,idx))
    while queue:
        _, cost, idx=heapq.heappop(queue)
        if cost != dist.get(idx):
            continue
        if idx==goal:
            path=[]
            while idx is not None:
                path.append((idx%NX,idx//NX))
                idx=prev[idx]
            return path[::-1]
        x,y=idx%NX,idx//NX
        for dx,dy in MOVES:
            xx,yy=x+dx,y+dy
            if not (0<=xx<NX and 0<=yy<NY):
                continue
            nxt=yy*NX+xx
            if pixels[nxt] and nxt!=goal:
                continue
            if dx and dy and (pixels[y*NX+xx] or pixels[yy*NX+x]):
                continue
            value=cost+(1.41421356237 if dx and dy else 1.0)
            old=prev.get(idx)
            if old is not None:
                odx,ody=x-old%NX,y-old//NX
                if (odx,ody)!=(dx,dy):
                    value+=0.08
            if value<dist.get(nxt,float("inf")):
                dist[nxt]=value
                prev[nxt]=idx
                heapq.heappush(queue,(value+heuristic(xx,yy),value,nxt))
    return None


def route(board):
    nodes={}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNumber() and pad.GetNetname() and not pad.GetNetname().startswith("unconnected-"):
                nodes.setdefault(pad.GetNetname(),[]).append(
                    (round(mm(pad.GetPosition().x)/STEP),round(mm(pad.GetPosition().y)/STEP)))
    nodes={k:v for k,v in nodes.items() if len(v)>1}
    masks={name:static_mask(board,name) for name in nodes}
    first=["/I2C_SCL","/I2C_SDA","/+3V3","/GND","/+5V_USB","/SENSOR_SIG"]
    orders=[first]
    rng=random.Random(42)
    others=list(itertools.permutations(first))
    rng.shuffle(others)
    orders.extend(others[:159])
    began=time.monotonic()
    best=0
    for attempt, order in enumerate(orders,1):
        routes={}
        for name in order:
            mask=masks[name].copy()
            draw=ImageDraw.Draw(mask)
            for other_paths in routes.values():
                for path in other_paths:
                    draw.line(path,fill=1,width=12)
                    for x,y in [path[0],path[-1]]:
                        draw.ellipse((x-6,y-6,x+6,y+6),fill=1)
            remaining=list(nodes[name])
            tree={remaining.pop(0)}
            paths=[]
            while remaining:
                target=min(remaining,key=lambda t:min(abs(t[0]-a)+abs(t[1]-b) for a,b in tree))
                path=route_path(mask,tree,target)
                if path is None:
                    break
                paths.append(path)
                tree.update(path)
                remaining.remove(target)
            if remaining:
                break
            routes[name]=paths
        best=max(best,len(routes))
        if len(routes)==len(nodes):
            print(f"Routed all {len(nodes)} nets in attempt {attempt}, {time.monotonic()-began:.1f}s",flush=True)
            return routes
        if attempt%8==0:
            print(f"Routing attempt {attempt}: best {best}/{len(nodes)} nets",flush=True)
    raise RuntimeError(f"No complete single-layer route found; best {best}/{len(nodes)} nets")


def tracks(board, routes):
    byname={net.GetNetname():net for net in board.GetNetInfo().NetsByNetcode().values()}
    for name, paths in routes.items():
        for path in paths:
            points=[path[0]]
            last=None
            for a,b in zip(path,path[1:]):
                direction=(b[0]-a[0],b[1]-a[1])
                if last is not None and direction!=last:
                    points.append(a)
                last=direction
            points.append(path[-1])
            for a,b in zip(points,points[1:]):
                if a==b:
                    continue
                item=p.PCB_TRACK(board)
                item.SetStart(pt(a[0]*STEP,a[1]*STEP))
                item.SetEnd(pt(b[0]*STEP,b[1]*STEP))
                item.SetWidth(p.FromMM(TRACE))
                item.SetLayer(p.B_Cu)
                item.SetNet(byname[name])
                board.Add(item)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--overwrite",action="store_true")
    args=parser.parse_args()
    target=ROOT/"WaterPressureSensor.kicad_pcb"
    if target.exists() and not args.overwrite:
        parser.error("PCB exists; save editor changes and use --overwrite explicitly")
    board=make_board()
    routes=route(board)
    tracks(board,routes)
    p.SaveBoard(str(target),board)
    (ROOT/"review/routes.json").write_text(json.dumps({"grid_mm":STEP,"routes":routes})+"\n",encoding="utf-8")
    print("Saved routed PCB:",target,flush=True)


if __name__=="__main__":
    main()
