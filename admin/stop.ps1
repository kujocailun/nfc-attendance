$ErrorActionPreference = 'Stop'
try
{
    $attendanceSession = Invoke-RestMethod -Uri 'http://127.0.0.1:8765/api/bootstrap' -TimeoutSec 3
    $attendanceHeaders = @{'X-Admin-Token' = $attendanceSession.token}
    Invoke-RestMethod -Uri 'http://127.0.0.1:8765/api/shutdown' -Method Post -Headers $attendanceHeaders -ContentType 'application/json' -Body '{}' -TimeoutSec 5 | Out-Null
    Write-Host 'Attendance admin stopped. The terminal can continue working offline.'
}
catch
{
    Write-Host 'No running admin server found, or it could not be stopped.'
}
