"""Check the exported functional netlist against the firmware pin table."""

import json
import re
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
design = json.loads((ROOT / "design.json").read_text(encoding="utf-8"))
tree = ET.parse(ROOT / "review/netlist.xml")
nets = {
    item.attrib["name"].lstrip("/"): {
        (node.attrib["ref"], node.attrib["pin"]) for node in item.findall("node")
    }
    for item in tree.findall("./nets/net")
}
checks = []


def require(condition, description):
    if not condition:
        raise SystemExit("Netlist mismatch: " + description)
    checks.append(description)


for name, mapping in design["firmware_nets"].items():
    require(("U1", mapping["symbol_pin"]) in nets.get(name, set()),
            f"{name}: U1.{mapping['symbol_pin']} / {mapping['controller_pin']}")

for name, members in {
    "I2C_SCL": {("U1", "14"), ("J1", "3")},
    "I2C_SDA": {("U1", "13"), ("J1", "4")},
    "GND": {("U1", "10"), ("J1", "1"), ("J2", "2")},
}.items():
    require(nets[name] == members, f"{name}: exact functional OLED connection")

require(nets["SENSOR_SIG"] == {("U1", "2"), ("J2", "4")},
        "RJ-10 contact 4 connects directly to U1.A0 without conditioning")
require(nets["+5V_USB"] == {("U1", "9"), ("J2", "1")},
        "RJ-10 contact 1 connects to WeMos USB 5V")
require(nets["+3V3"] == {("U1", "8"), ("J1", "2")},
        "OLED contact 2 connects to the WeMos 3.3V regulator output")
for pin, name in design["confirmed"]["rj10_pinout"].items():
    require(("J2", pin) in nets[name], f"Confirmed cable assignment: J2.{pin} = {name}")
for name, nodes in nets.items():
    if ("J2", "3") in nodes:
        require(name.startswith("unconnected-") and len(nodes) == 1,
                "RJ-10 contact 3 is reserved and has no carrier connection")
require(any(node.attrib["ref"] == "J2" and node.attrib["pin"] == "3"
            and "no_connect" in node.attrib.get("pintype", "")
            for node in tree.findall("./nets/net/node")),
        "RJ-10 contact 3 is explicitly unused on the carrier")
require(not any(name.startswith("unconnected-") and any(ref == "J1" for ref, _ in nodes)
                for name, nodes in nets.items()), "All four OLED contacts are connected")
components = tree.findall("./components/comp")
require(tree.find("./components/comp[@ref='J2']/libsource").attrib["part"] == "4P4C",
        "J2 uses the standard four-contact 4P4C connector symbol")
require({pin for nodes in nets.values() for ref, pin in nodes if ref == "J2"} == {"1", "2", "3", "4"},
        "RJ-10 has exactly contacts 1-4")
require({part.attrib["ref"] for part in components} == {"U1", "J1", "J2"},
        "Only the three requested module/interface blocks exist")
require(all((part.findtext("footprint") or "").strip() == design["footprints"][part.attrib["ref"]]
            for part in components), "All three footprints match the selected local library")
config = (ROOT.parent / "src/Config.h").read_text(encoding="utf-8")
require(bool(re.search(r"#define\s+SENSOR_PIN\s+A0\b", config)), "Firmware selects A0")
require(bool(re.search(r"#define\s+OLED_RESET\s+-1\b", config)), "OLED uses no Reset GPIO")

erc = json.loads((ROOT / "review/erc.json").read_text(encoding="utf-8"))
violations = [v for sheet in erc["sheets"] for v in sheet["violations"]]
counts = Counter(v["type"] for v in violations)
require(not counts, "ERC has no errors or warnings and no exclusions are required")
report = {
    "revision": design["revision"],
    "functional_netlist_checks_passed": True,
    "checks": checks,
    "erc_passed": True,
    "erc_errors": sum(v["severity"] == "error" for v in violations),
    "erc_warnings": sum(v["severity"] == "warning" for v in violations),
    "erc_types": dict(counts),
    "board_created": (ROOT / "WaterPressureSensor.kicad_pcb").exists(),
    "manufacturing_ready": False,
    "reason": "Electrical connections checked; physical module fit must be checked before fabrication",
}
(ROOT / "review/verification.json").write_text(
    json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Functional netlist: {len(checks)} checks matched the intended draft.")
print(f"ERC: {report['erc_errors']} errors, {report['erc_warnings']} warnings.")
print("No PCB/fabrication readiness is claimed.")
