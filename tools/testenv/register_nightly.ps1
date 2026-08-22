# Registers (or removes) the nightly full-suite proving-ground run as a scheduled task.
# The task runs at 04:00 when the machine is on and idle, against whatever build is deployed
# in C:\Program Files\Wind - i.e. latest deployed main. Results land in tools\testenv\results.
#
#   powershell -File tools\testenv\register_nightly.ps1            # install/update
#   powershell -File tools\testenv\register_nightly.ps1 -Remove
param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$name = 'Wind proving ground nightly'
if ($Remove) {
  Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
  Write-Host "Removed '$name'."
  return
}
$runner = Join-Path $PSScriptRoot 'run.ps1'
$action  = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$runner`" -Suite full -CI"
$trigger = New-ScheduledTaskTrigger -Daily -At 04:00
$settings = New-ScheduledTaskSettingsSet -RunOnlyIfIdle -IdleDuration (New-TimeSpan -Minutes 10) `
  -IdleWaitTimeout (New-TimeSpan -Hours 2) -StartWhenAvailable
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Write-Host "Registered '$name' (daily 04:00, only when idle; results in tools\testenv\results)."
