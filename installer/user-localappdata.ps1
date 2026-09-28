<#
    Prints the %LOCALAPPDATA% folder of the user who owns the desktop in this session (issue #274).

    The uninstaller runs elevated, and an elevated process's $LOCALAPPDATA belongs to whichever
    account approved the UAC prompt. When a standard user elevates with an administrator's
    credentials that is the ADMIN's profile, so "remove Wind's settings" looked in the wrong place
    and the real user's %LOCALAPPDATA%\Wind was silently left behind. The shell (explorer.exe) in
    our own session always runs as the signed-in user, so its owner is the answer - the same
    reasoning that makes setup launch Wind through explorer.exe (CLAUDE.md).

    Output: the folder on stdout (nothing else), exit 0. Exit 1 with nothing on stdout when it
    cannot be determined; the uninstaller then falls back to its own $LOCALAPPDATA.
#>
$ErrorActionPreference = 'Stop'
# Same pin as local-sign.ps1 (issue #265): started from a pwsh 7 window, the inherited module
# path would break Get-CimInstance and the answer would silently fall back to the wrong folder.
$env:PSModulePath = "$env:SystemRoot\system32\WindowsPowerShell\v1.0\Modules;$env:ProgramFiles\WindowsPowerShell\Modules"
try {
    $session = (Get-Process -Id $PID).SessionId
    $sid = $null
    foreach ($p in Get-CimInstance Win32_Process -Filter "Name='explorer.exe'") {
        if ($p.SessionId -ne $session) { continue }
        $o = Invoke-CimMethod -InputObject $p -MethodName GetOwnerSid
        if ($o.Sid) { $sid = $o.Sid; break }
    }
    if (-not $sid) { exit 1 }

    # Same account as ours: the ordinary answer, including any folder redirection.
    if ($sid -eq [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) {
        [Console]::Out.Write([Environment]::GetFolderPath('LocalApplicationData'))
        exit 0
    }

    $profile = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid").ProfileImagePath
    $profile = [Environment]::ExpandEnvironmentVariables($profile)
    if (-not $profile) { exit 1 }
    # The user is signed in, so their hive is loaded: honour a redirected Local AppData if set.
    $local = $null
    $usf = "Registry::HKEY_USERS\$sid\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
    if (Test-Path $usf) {
        $raw = (Get-ItemProperty $usf -ErrorAction SilentlyContinue).'Local AppData'
        if ($raw) { $local = $raw.Replace('%USERPROFILE%', $profile) }
    }
    if (-not $local) { $local = Join-Path $profile 'AppData\Local' }
    [Console]::Out.Write($local)
    exit 0
} catch {
    exit 1
}
