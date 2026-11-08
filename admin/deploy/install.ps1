param([string]$PythonPath = '')
$ErrorActionPreference = 'Stop'
$attendanceRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $attendanceRoot
if (-not $PythonPath)
{
    $attendanceLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($attendanceLauncher)
    {
        $PythonPath = (& $attendanceLauncher.Source -3.11 -c 'import sys; print(sys.executable)' 2>$null)
    }
    if (-not $PythonPath)
    {
        $attendanceCommand = Get-Command python -ErrorAction SilentlyContinue
        if ($attendanceCommand) { $PythonPath = $attendanceCommand.Source }
    }
}
if (-not $PythonPath)
{
    throw 'Install Python 3.11 x64 first: https://www.python.org/downloads/windows/'
}
& $PythonPath -c 'import sys; assert sys.version_info[:2] == (3,11) and sys.maxsize > 2**32'
if ($LASTEXITCODE -ne 0) { throw 'Select Python 3.11 x64 using -PythonPath.' }
if (-not (Test-Path -LiteralPath '.venv\Scripts\python.exe'))
{
    & $PythonPath -m venv .venv
    if ($LASTEXITCODE -ne 0) { throw 'Creating virtual environment failed.' }
}
if (Test-Path -LiteralPath 'wheelhouse')
{
    & '.\.venv\Scripts\python.exe' -m pip install --no-index --find-links wheelhouse -r requirements-lock.txt
}
else
{
    & '.\.venv\Scripts\python.exe' -m pip install -r requirements-lock.txt
}
if ($LASTEXITCODE -ne 0) { throw 'Installing dependencies failed.' }
New-Item -ItemType Directory -Path 'data','deploy-state' -Force | Out-Null
if (-not (Test-Path -LiteralPath 'network-config.json'))
{
    Copy-Item -LiteralPath 'network-config.example.json' -Destination 'network-config.json'
}
if (-not (Test-Path -LiteralPath 'data\attendance.sqlite3'))
{
    if (Test-Path -LiteralPath 'migration\attendance.sqlite3')
    {
        Copy-Item -LiteralPath 'migration\attendance.sqlite3' -Destination 'data\attendance.sqlite3'
    }
}
Write-Host 'Installed. Configure Cloudflare Access, then run deploy\prepare.py before publishing hostnames.'
