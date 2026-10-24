"""Generate the schematic with the confirmed RJ-10 and OLED pin assignments."""

import argparse
import json
import re
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = Path(r"C:\Program Files\KiCad\10.0\share\kicad\symbols")
PROJECT = "WaterPressureSensor"
FOOTPRINTS = json.loads((ROOT / "design.json").read_text(encoding="utf-8")).get("footprints", {})


def uid(name):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "wps-hardware-a0/" + name))


def quote(value):
    return json.dumps(value, ensure_ascii=False)


def block(source, start):
    depth, quoted, escaped = 0, False, False
    for index in range(start, len(source)):
        char = source[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError("Unbalanced library symbol")


def library_symbol(filename, name):
    source = (LIB / filename).read_text(encoding="utf-8")
    start = source.index('(symbol "' + name + '"')
    symbol = block(source, start)
    # Physical footprint selection must await the actual module revision.
    symbol = re.sub(r'(\(property "Footprint"\s+)"[^"]*"', r'\1""', symbol)
    return symbol


def effects(size=1.27, justify="", hide=False):
    return (f'(effects (font (size {size} {size}))'
            + (f' (justify {justify})' if justify else "")
            + (' (hide yes)' if hide else "") + ')')


def prop(name, value, x, y, hide=False):
    return f'(property {quote(name)} {quote(value)} (at {x:g} {y:g} 0) {effects(hide=hide)})'


def simple_symbol(name, value, reference, pins, half_height=10.16):
    parts = [f'(symbol {quote(name)} (pin_names (offset 1.016))',
             '(exclude_from_sim no) (in_bom yes) (on_board yes)',
             prop("Reference", reference, 0, half_height + 2.54),
             prop("Value", value, 0, -half_height - 2.54),
             prop("Footprint", "", 0, 0, True),
             prop("Datasheet", "", 0, 0, True),
             prop("Description", "Module connector; footprint pending mechanical verification", 0, 0, True),
             f'(symbol {quote(name + "_0_1")} (rectangle (start -12.7 {half_height}) '
             f'(end 12.7 {-half_height}) (stroke (width 0.254) (type default)) (fill (type background))))',
             f'(symbol {quote(name + "_1_1")}']
    for number, pin_name, kind, y in pins:
        parts.append(f'(pin {kind} line (at -17.78 {y:g} 0) (length 5.08) '
                     f'(name {quote(pin_name)} {effects(1.016)}) '
                     f'(number {quote(number)} {effects(1.016)}))')
    parts.append('))')
    return "\n".join(parts)


def placed(name, reference, value, x, y, pins, hidden=False):
    lines = [f'(symbol (lib_id {quote(PROJECT + ":" + name)}) (at {x:g} {y:g} 0) (unit 1)',
             '(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)',
             f'(uuid "{uid(reference)}")',
             prop("Reference", reference, x, y - 23 if name == "WEMOS_D1_mini" else y - 14, hidden),
             prop("Value", value, x, y + 23 if name == "WEMOS_D1_mini" else y + 14, hidden),
             prop("Footprint", FOOTPRINTS.get(reference, ""), x, y, True),
             prop("Datasheet", "", x, y, True),
             prop("Description", "", x, y, True)]
    for number in pins:
        lines.append(f'(pin {quote(number)} (uuid "{uid(reference + "/pin/" + number)}"))')
    lines.append(f'(instances (project {quote(PROJECT)} (path "/{uid("sheet")}" '
                 f'(reference {quote(reference)}) (unit 1)))))')
    return "\n".join(lines)


def label(name, x, y, side="left"):
    return f'(label {quote(name)} (at {x:g} {y:g} 0) {effects(1.016, side + " bottom")} (uuid "{uid(f"label/{name}/{x}/{y}")}"))'


def wire(x1, y1, x2, y2):
    return f'(wire (pts (xy {x1:g} {y1:g}) (xy {x2:g} {y2:g})) (stroke (width 0) (type default)) (uuid "{uid(f"wire/{x1}/{y1}/{x2}/{y2}")}"))'


def text(value, x, y, size=1.27):
    return f'(text {quote(value)} (at {x:g} {y:g} 0) {effects(size, "left top")} (uuid "{uid("text/" + value)}"))'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    target = ROOT / f"{PROJECT}.kicad_sch"
    if target.exists() and not args.overwrite:
        parser.error("Schematic already exists; use --overwrite only after saving editor changes")
    ROOT.mkdir(parents=True, exist_ok=True)
    (ROOT / "review").mkdir(exist_ok=True)
    design = json.loads((ROOT / "design.json").read_text(encoding="utf-8"))
    wemos = library_symbol("RF_Module.kicad_sym", "WEMOS_D1_mini")
    flag = library_symbol("power.kicad_sym", "PWR_FLAG")
    oled_pins = [("1", "GND", "power_in", 7.62),
                 ("2", "VCC", "power_in", 2.54),
                 ("3", "SCL", "input", -2.54),
                 ("4", "SDA", "bidirectional", -7.62)]
    oled = simple_symbol("OLED_SSD1306_I2C", "SSD1306 0.96in I2C", "J", oled_pins)
    rj_pins = [str(n) for n in range(1, 5)]
    rj = library_symbol("Connector.kicad_sym", "4P4C")
    symbols = {"WEMOS_D1_mini": wemos, "OLED_SSD1306_I2C": oled,
               "4P4C": rj, "PWR_FLAG": flag}
    header = '(kicad_symbol_lib (version 20251024) (generator "kicad_symbol_editor") (generator_version "10.0")\n'
    (ROOT / f"{PROJECT}.kicad_sym").write_text(header + "\n".join(symbols.values()) + ")\n", encoding="utf-8")
    (ROOT / "sym-lib-table").write_text(
        '(sym_lib_table (version 7) (lib (name "WaterPressureSensor") (type "KiCad") '
        '(uri "${KIPRJMOD}/WaterPressureSensor.kicad_sym") (options "") (descr "Local functional draft symbols")))\n', encoding="utf-8")
    pro = ROOT / f"{PROJECT}.kicad_pro"
    if not pro.exists():
        pro.write_text(json.dumps({"meta": {"filename": pro.name, "version": 1},
                                   "text_variables": {"HW_REV": "A1 ELECTRICAL DRAFT"}}, indent=2) + "\n", encoding="utf-8")
    body = [f'(kicad_sch (version 20250610) (generator "eeschema") (generator_version "10.0") (uuid "{uid("sheet")}") (paper "A4")',
            '(title_block (title "WaterPressureSensor - module carrier") (date "2026-10-17") (rev "A2 REVIEW") '
            '(comment 1 "Electrical connections defined; mechanical verification pending"))', '(lib_symbols']
    for name, symbol in symbols.items():
        body.append(symbol.replace(f'(symbol "{name}"', f'(symbol "{PROJECT}:{name}"', 1))
    body += [')', text("WaterPressureSensor / Single-sided module carrier", 20.32, 17.78, 2.0),
             text("A2: SINGLE-SIDED PCB REVIEW - VERIFY MECHANICS BEFORE FABRICATION", 20.32, 25.4, 1.5),
             text("USB on U1 is the only power input. Direct sensor signal to A0.", 20.32, 33.02),
             placed("WEMOS_D1_mini", "U1", "WeMos D1 Mini", 73.66, 83.82, [str(n) for n in range(1, 17)]),
             placed("OLED_SSD1306_I2C", "J1", "SSD1306 / tall 1x4 socket", 163.83, 77.47, [p[0] for p in oled_pins]),
             placed("4P4C", "J2", "RJ-10 / 4P4C PCB jack", 246.38, 85.09, rj_pins)]
    # U1 symbol coordinates are transformed from library Y-up to sheet Y-down.
    positions = {"2": (83.82, 71.12), "8": (76.2, 63.5), "9": (71.12, 63.5),
                 "10": (73.66, 104.14), "13": (83.82, 78.74), "14": (83.82, 76.2)}
    nets = {"2": "SENSOR_SIG", "8": "+3V3", "9": "+5V_USB", "10": "GND", "13": "I2C_SDA", "14": "I2C_SCL"}
    for pin, (x, y) in positions.items():
        if pin == "8":
            ex, ey = x, 50.8
        elif pin == "9":
            ex, ey = x, 55.88
        elif pin == "10":
            ex, ey = x, 116.84
        else:
            ex, ey = 111.76, y
        body += [wire(x, y, ex, ey), label(nets[pin], ex, ey)]
    unused = {"1": (63.5, 73.66), "3": (83.82, 73.66), "4": (83.82, 86.36),
              "5": (83.82, 88.9), "6": (83.82, 91.44), "7": (83.82, 93.98),
              "11": (83.82, 83.82), "12": (83.82, 81.28), "15": (63.5, 81.28), "16": (63.5, 83.82)}
    for pin, (x, y) in unused.items():
        body.append(f'(no_connect (at {x:g} {y:g}) (uuid "{uid("nc/U1/" + pin)}"))')
    # Connector:4P4C pin ends, as drawn in the local KiCad symbol.
    rj_positions = {"1": (256.54, 87.63), "2": (256.54, 85.09),
                    "3": (256.54, 82.55), "4": (256.54, 80.01)}
    for pin, net in design["confirmed"]["rj10_pinout"].items():
        x, y = rj_positions[pin]
        body += [wire(x, y, 269.24, y), label(net, 269.24, y)]
    # Only contacts 1, 2 and 4 were requested. Contact 3 is reserved/unused on this carrier.
    x, y = rj_positions["3"]
    body.append(f'(no_connect (at {x:g} {y:g}) (uuid "{uid("nc/J2/3")}"))')
    for pin, name, _, dy in oled_pins:
        x, y = 146.05, 77.47 - dy
        net = {"1": "GND", "2": design["design_choices"]["oled_supply_net"],
               "3": "I2C_SCL", "4": "I2C_SDA"}[pin]
        body += [wire(130.81, y, x, y), label(net, 130.81, y)]
    for ref, net, x in [("#FLG01", "+5V_USB", 38.1), ("#FLG02", "GND", 55.88)]:
        body += [placed("PWR_FLAG", ref, "PWR_FLAG", x, 128.27, ["1"], True),
                 wire(x, 128.27, x, 133.35), label(net, x, 133.35)]
    body += [text("USB power source in U1", 25.4, 140.97, 1.016),
             text("OLED: second tier OVER WeMos; tall 1x4 socket\nFront view, left to right: 1 GND, 2 VCC, 3 SCL, 4 SDA\nVCC = 3.3V from U1; reference envelope 27 x 27 x 3.5mm", 129.54, 105.41, 1.016),
             text("J2: RJ-10 / 4P4C PCB-mounted socket\n1: +5V_USB   2: GND   4: SENSOR_SIG to U1.A0\n3: reserved / unused on carrier", 203.2, 110.49, 1.016),
             text("MECHANICAL CHECKS BEFORE FABRICATION\n1. Type-C WeMos header fit and OLED header pitch/offset\n2. Actual RJ-10 jack versus selected EVERCOM family\n3. Elevated socket mating height and RF clearance", 20.32, 154.94, 1.016),
             text("PCB intent: B.Cu only; through-hole assembly\nNo external analog conditioning, per user requirement\nRJ-10 pin 3 and unused MCU pins are NC; no ERC exclusions", 157.48, 151.13, 1.016),
             '(sheet_instances (path "/" (page "1"))) (embedded_fonts no))']
    target.write_text("\n".join(body) + "\n", encoding="utf-8")
    print("Generated logical schematic:", target)
    print("Physical pinouts, footprints and PCB remain pending; no manufacturing outputs generated.")


if __name__ == "__main__":
    main()
