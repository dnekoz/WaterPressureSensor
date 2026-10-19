param([string]$KiCadBin = 'C:\Program Files\KiCad\10.0\bin')
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskCli = Join-Path $KiCadBin 'kicad-cli.exe'
$taskPython = Join-Path $KiCadBin 'python.exe'
$taskSch = Join-Path $taskRoot 'WaterPressureSensor.kicad_sch'
$taskPcb = Join-Path $taskRoot 'WaterPressureSensor.kicad_pcb'
$taskReview = Join-Path $taskRoot 'review'
$taskExport = Join-Path $taskRoot 'exports\A2-review'
New-Item -ItemType Directory -Path $taskExport -Force | Out-Null
New-Item -ItemType Directory -Path $taskReview -Force | Out-Null
function Invoke-KiCad([string[]]$TaskArguments) {
    & $taskCli @TaskArguments
    if ($LASTEXITCODE -ne 0) { throw "KiCad failed ($LASTEXITCODE): $($TaskArguments -join ' ')" }
}
Invoke-KiCad @('sch','erc','--exit-code-violations','--format','json','--output',"$taskReview\erc.json",$taskSch)
Invoke-KiCad @('sch','export','netlist','--format','kicadxml','--output',"$taskReview\netlist.xml",$taskSch)
& $taskPython (Join-Path $PSScriptRoot 'check_netlist.py')
if ($LASTEXITCODE -ne 0) { throw 'Netlist audit failed' }
Invoke-KiCad @('pcb','drc','--schematic-parity','--exit-code-violations','--format','json','--output',"$taskReview\drc.json",$taskPcb)
Invoke-KiCad @('sch','export','pdf','--output',"$taskReview\schematic.pdf",$taskSch)
Invoke-KiCad @('pcb','export','pdf','--layers','B.Cu,F.SilkS,Edge.Cuts,Dwgs.User','--mode-single','--output',"$taskReview\board-layout.pdf",$taskPcb)
Invoke-KiCad @('pcb','export','pdf','--layers','F.SilkS,F.Fab,Edge.Cuts,Dwgs.User','--black-and-white','--sketch-pads-on-fab-layers','--mode-single','--output',"$taskReview\fit-check-1to1.pdf",$taskPcb)
Invoke-KiCad @('pcb','export','pdf','--layers','B.Cu,Edge.Cuts','--mirror','--black-and-white','--mode-single','--output',"$taskReview\copper-bottom-view.pdf",$taskPcb)
Invoke-KiCad @('pcb','export','svg','--layers','B.Cu,F.SilkS,Edge.Cuts,Dwgs.User','--mode-single','--fit-page-to-board','--output',"$taskReview\board-layout.svg",$taskPcb)
Invoke-KiCad @('pcb','export','gerbers','--layers','B.Cu,B.Mask,F.SilkS,Edge.Cuts','--output',($taskExport+'\'),$taskPcb)
Invoke-KiCad @('pcb','export','drill','--format','excellon','--excellon-units','mm','--generate-map','--map-format','pdf','--generate-report','--report-path',"$taskExport\drill-report.txt",'--output',($taskExport+'\'),$taskPcb)
Invoke-KiCad @('pcb','export','step','--board-only','--force','--output',"$taskExport\WaterPressureSensor-carrier-only.step",$taskPcb)
@'
Reference,Quantity,Description,Part_number,Assembly_note
U1,1,WeMos D1 Mini USB Type-C,User supplied module,Check actual 2x8 header fit before fabrication
U1 sockets,2,Female socket 1x8 2.54mm H8.5mm,Generic compatible socket,1.0mm carrier drill; verify pin size
J1,1,Elevated OLED socket 1x4 2.54mm,Samtec ESW-104-44-G-S,18.67mm body; verify OLED mating and underside clearance
J2,1,RJ10 4P4C right-angle PCB jack,EVERCOM 5301-4P4C / 5301-440xxx family,Exact delivered part drawing must match the footprint
OLED,1,0.96in SSD1306 128x64 I2C module,User supplied module,27x27mm reference envelope; 1 GND 2 VCC 3 SCL 4 SDA
H1-H4,4,Mounting clearance holes 3.2mm M3,Mechanical holes,No purchased electronic parts
'@ | Set-Content -LiteralPath "$taskExport\BOM.csv" -Encoding utf8
@'
A2 REVIEW EXPORT -- NOT RELEASED FOR FABRICATION

Carrier: 46 x 65 mm, 1.6 mm substrate, assumed 35 um copper.
ONE physical copper side: B.Cu (bottom). No F.Cu Gerber is supplied.
Tracks 0.4 mm; clearance 0.25 mm; no vias or wire jumpers.
KiCad uses a two-layer minimum stack and *.Cu through-hole pad definitions.
Only the B.Cu file is to be fabricated as copper. All solder joints are accessible
from the bottom; drill metallization is not needed for connectivity.
The Excellon file combines all 30 holes. Drill report identifies diameters.

Check the actual Type-C WeMos header geometry, the OLED 2.54 mm pin pitch/edge
offset and mated height, and the exact RJ10 jack footprint before ordering.
The included fit-check-1to1.pdf must be printed at 100%; verify the 10 mm ticks.
STEP contains the bare carrier only. It does not validate the module assembly.
BOM records the modules supplied by the user and unresolved exact supplier parts.
'@ | Set-Content -LiteralPath "$taskExport\README.txt" -Encoding utf8
& $taskPython (Join-Path $PSScriptRoot 'check_board.py')
if ($LASTEXITCODE -ne 0) { throw 'PCB/export audit failed' }
Copy-Item -LiteralPath "$taskReview\board-verification.json","$taskReview\erc.json","$taskReview\drc.json","$taskReview\schematic.pdf","$taskReview\fit-check-1to1.pdf" -Destination $taskExport
$taskNative = Join-Path $taskExport 'KiCad'
New-Item -ItemType Directory -Path $taskNative -Force | Out-Null
foreach ($taskFile in @('WaterPressureSensor.kicad_pro','WaterPressureSensor.kicad_sch','WaterPressureSensor.kicad_pcb','WaterPressureSensor.kicad_sym','sym-lib-table','fp-lib-table','design.json','README.md')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination $taskNative
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'WaterPressureSensor.pretty'),(Join-Path $taskRoot 'references') -Destination $taskNative -Recurse -Force
$taskNativeTools = Join-Path $taskNative 'tools'
New-Item -ItemType Directory -Path $taskNativeTools -Force | Out-Null
Get-ChildItem -LiteralPath $PSScriptRoot -File | Where-Object { $_.Extension -in @('.py','.ps1') } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $taskNativeTools
}
$taskArchive = Join-Path $taskRoot 'exports\WaterPressureSensor-A2-review.zip'
$taskZipStream = [System.IO.File]::Open($taskArchive, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$taskZip = [System.IO.Compression.ZipArchive]::new($taskZipStream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -LiteralPath $taskExport -File -Recurse |
        Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' -and $_.Extension -ne '.pyc' } |
        ForEach-Object {
            $taskEntry = [System.IO.Path]::GetRelativePath((Split-Path -Parent $taskExport), $_.FullName).Replace('\','/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskZip, $_.FullName, $taskEntry) | Out-Null
        }
} finally {
    $taskZip.Dispose()
    $taskZipStream.Dispose()
}
Write-Output "Review archive: $taskArchive"
