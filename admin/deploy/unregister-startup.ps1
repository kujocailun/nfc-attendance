$ErrorActionPreference = 'Stop'
Stop-ScheduledTask -TaskName 'NFC-Attendance-Backend' -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'NFC-Attendance-Backend' -Confirm:$false
Write-Host 'Startup task removed. Database files are preserved.'
