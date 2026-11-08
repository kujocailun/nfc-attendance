$ErrorActionPreference = 'Stop'
$attendanceRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $attendanceRoot
$attendancePython = Join-Path $attendanceRoot '.venv\Scripts\python.exe'
while ($true)
{
    & $attendancePython (Join-Path $attendanceRoot 'network_server.py')
    Start-Sleep -Seconds 5
}
