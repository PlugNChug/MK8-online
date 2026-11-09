$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$launcher = Join-Path $projectRoot 'target/release/Launcher.exe'
if (-not (Test-Path -LiteralPath $launcher)) { throw 'Build the development launcher first.' }
$previous = $env:MK8_NEXTENDO_TEST
try {
    $env:MK8_NEXTENDO_TEST = '1'
    Start-Process -FilePath $launcher -WorkingDirectory $projectRoot
    Write-Host 'Nextendo test mode: Play solo, F1 > Online > Nextendo account. Sign in, select Nextendo and restart from the overlay after saving. Tokens last for this game process only.'
} finally {
    $env:MK8_NEXTENDO_TEST = $previous
}
