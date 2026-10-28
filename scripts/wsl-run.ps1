param(
    [ValidateSet('Test', 'Build', 'Qemu', 'Gdb', 'All')]
    [string]$Task = 'All'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ($projectRoot -notmatch '^([A-Za-z]):\\(.*)$') { throw 'Expected a Windows drive path' }
$linuxSource = '/mnt/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2].Replace('\', '/')
& wsl -d Ubuntu -u root -- bash "$linuxSource/scripts/wsl-run.sh" "$linuxSource" $Task.ToLowerInvariant()
if ($LASTEXITCODE -ne 0) { throw "WSL project task failed: $Task" }
