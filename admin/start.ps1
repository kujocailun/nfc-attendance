$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$attendanceCandidates = @(
    (Join-Path $PSScriptRoot '.venv\Scripts\python.exe'),
    (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'),
    (Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
)
$attendancePython = $null
foreach ($attendanceCandidate in $attendanceCandidates)
{
    if ($attendanceCandidate -and (Test-Path -LiteralPath $attendanceCandidate))
    {
        & $attendanceCandidate -c 'import serial' 2>$null
        if ($LASTEXITCODE -eq 0)
        {
            $attendancePython = $attendanceCandidate
            break
        }
    }
}
if (-not $attendancePython)
{
    Write-Host 'Python 3.10+ with pyserial is required. Run: python -m pip install pyserial==3.5'
    Read-Host 'Press Enter to close'
    exit 1
}
try
{
    $attendanceReady = Invoke-RestMethod -Uri 'http://127.0.0.1:8765/api/bootstrap' -TimeoutSec 2
    if ($attendanceReady.version -eq '1.0.0')
    {
        Start-Process 'http://127.0.0.1:8765'
        exit 0
    }
}
catch
{
    # Start a new local server when no existing instance answers.
}
& $attendancePython (Join-Path $PSScriptRoot 'server.py') --open
if ($LASTEXITCODE -ne 0)
{
    Read-Host 'Server stopped. Press Enter to close'
}
