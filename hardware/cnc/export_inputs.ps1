param([string]$KiCadBin='C:\Program Files\KiCad\10.0\bin')
$ErrorActionPreference='Stop'
$taskDir=$PSScriptRoot
$taskCli=Join-Path $KiCadBin 'kicad-cli.exe'
$taskPython=Join-Path $KiCadBin 'python.exe'
& $taskPython (Join-Path $taskDir 'prepare_jobs.py')
if ($LASTEXITCODE -ne 0) { throw 'CAM preparation failed' }
$taskBoard=Join-Path $taskDir 'inputs\WaterPressureSensor-CAM.kicad_pcb'
$taskOutput=(Join-Path $taskDir 'inputs')+'\'
& $taskCli pcb export gerbers --layers 'B.Cu,Edge.Cuts' --precision 6 --use-drill-file-origin --no-x2 --output $taskOutput $taskBoard
if ($LASTEXITCODE -ne 0) { throw 'Gerber export failed' }
& $taskCli pcb export drill --format excellon --drill-origin plot --excellon-units mm --excellon-zeros-format decimal --generate-map --map-format pdf --generate-report --report-path (Join-Path $taskOutput 'drill-report.txt') --output $taskOutput $taskBoard
if ($LASTEXITCODE -ne 0) { throw 'Drill export failed' }
# Deliberately omit --excellon-min-header and --excellon-mirror-y.
Write-Output 'Raw CAM inputs share the drill/place origin. They are NOT mirrored.'
