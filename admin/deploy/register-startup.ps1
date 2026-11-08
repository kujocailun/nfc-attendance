$ErrorActionPreference = 'Stop'
$attendanceRoot = Split-Path -Parent $PSScriptRoot
$attendanceConfig = Get-Content -LiteralPath (Join-Path $attendanceRoot 'network-config.json') -Raw | ConvertFrom-Json
if ($attendanceConfig.local_admin -ne $false -or -not $attendanceConfig.access_issuer -or -not $attendanceConfig.access_audience)
{
    throw 'Production Access configuration is required before registering startup.'
}
$attendanceIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$attendanceAdmin = New-Object Security.Principal.WindowsPrincipal($attendanceIdentity)
if (-not $attendanceAdmin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
{
    throw 'Run this script in an elevated PowerShell on the deployment PC.'
}
if (Get-ScheduledTask -TaskName 'NFC-Attendance-Backend' -ErrorAction SilentlyContinue)
{
    throw 'Task already exists. Inspect it before changing startup configuration.'
}
$attendanceAction = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + (Join-Path $PSScriptRoot 'run.ps1') + '"') -WorkingDirectory $attendanceRoot
$attendanceTrigger = New-ScheduledTaskTrigger -AtStartup
$attendancePrincipal = New-ScheduledTaskPrincipal -UserId $attendanceIdentity.Name -LogonType S4U -RunLevel Limited
$attendanceSettings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'NFC-Attendance-Backend' -Action $attendanceAction -Trigger $attendanceTrigger -Principal $attendancePrincipal -Settings $attendanceSettings | Out-Null
Start-ScheduledTask -TaskName 'NFC-Attendance-Backend'
Write-Host 'Startup task registered for the current Windows account. It runs without an interactive login.'
