"""Compact and reroute the user's saved PCB placement; never regenerate modules.

Use KiCad Python. The input is preserved; output requires --overwrite if present.
The A3 placement follows the user's RJ10 / OLED / WeMos arrangement and USB direction.
"""

import argparse
import json
import math
from pathlib import Path

import pcbnew as p
import build_board as routing

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 36.0, 60.0


def compact(source, target):
    board = p.LoadBoard(str(source))
    parts = {fp.GetReference(): fp for fp in board.GetFootprints()}
    # Preserve module orientations from the edited PCB. Only close the gaps and
    # shift OLED away from the transformed WeMos antenna rule area.
    expected_angles = {"U1": 90, "J1": 90, "J2": 180}
    for ref, angle in expected_angles.items():
        if abs(parts[ref].GetOrientationDegrees() - angle) > 0.001:
            raise ValueError(f"Unexpected {ref} orientation; re-evaluate placement")
    for ref, xy in {"U1": (8.72, 50.0), "J1": (18.0, 22.5),
                    "J2": (18.0, 15.0), "H1": (3.5, 3.5),
                    "H2": (32.5, 3.5), "H3": (3.5, 56.5),
                    "H4": (32.5, 56.5)}.items():
        parts[ref].SetPosition(routing.pt(*xy))
    # Old tracks, outline and loose annotations refer to the previous placement.
    # Footprints, nets, pads, UUIDs, user module edits and local library links stay.
    old_tracks, old_drawings = list(board.GetTracks()), list(board.GetDrawings())
    for item in old_tracks + old_drawings:
        board.Delete(item)
    routing.rectangle(board, 0, 0, WIDTH, HEIGHT, p.Edge_Cuts, 0.05)
    # OLED reference envelope: 27 x 27, centered on J1, 2 mm pin-to-edge offset.
    routing.rectangle(board, 8.31, 20.5, 35.31, 47.5, p.Dwgs_User)
    routing.label(board, "OLED 27x27 - upper tier", 22.0, 39.0, p.Dwgs_User, 0.8)
    routing.label(board, "J1 OLED", 21.8, 20.0, size=0.8)
    routing.label(board, "GND 3V3 SCL SDA", 21.81, 24.8, size=0.8)
    routing.label(board, "U1 D1 MINI", 19.0, 53.2, size=0.8)
    routing.label(board, "USB-C", 31.5, 52.8, size=0.8)
    routing.label(board, "RJ10 / 4P4C", 16.0, 3.0, size=0.8)
    routing.label(board, "1 +5V\n2 GND\n3 NC\n4 A0", 27.5, 12.0, size=0.8)
    routing.label(board, "WPS A3 / B.Cu", 18.0, 57.8, size=0.8)
    routing.line(board, (5,64), (15,64), p.Dwgs_User)
    for x in (5,15):
        routing.line(board, (x,63.5), (x,64.5), p.Dwgs_User)
    routing.label(board, "10 mm / print 100%", 25.0, 64.0, p.Dwgs_User, 0.7)
    routing.label(board, "36 x 60 mm - A3 fit check", 18.0, 67.0, p.Dwgs_User, 0.9)
    title = board.GetTitleBlock()
    title.SetTitle("WaterPressureSensor - A3 compact layout review")
    title.SetRevision("A3")
    routing.WIDTH, routing.HEIGHT = WIDTH, HEIGHT
    routing.NX = math.ceil(WIDTH/routing.STEP) + 1
    routing.NY = math.ceil(HEIGHT/routing.STEP) + 1
    routes = routing.route(board)
    routing.tracks(board, routes)
    p.SaveBoard(str(target), board)
    (ROOT/'review/routes.json').write_text(
        json.dumps({'revision':'A3','grid_mm':routing.STEP,'routes':routes})+'\n',
        encoding='utf-8')
    print(f"Saved {WIDTH:g} x {HEIGHT:g} mm compact PCB: {target}", flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'WaterPressureSensor.kicad_pcb')
    parser.add_argument('--overwrite', action='store_true')
    args = parser.parse_args()
    if args.output.exists() and not args.overwrite:
        parser.error('Output exists; save editor changes and use --overwrite explicitly')
    if args.source.resolve() == args.output.resolve():
        parser.error('Use the preserved input snapshot as --source')
    compact(args.source, args.output)
