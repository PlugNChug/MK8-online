# Windows verification for the current checkout.
#
# Run from a Windows 10/11 x64 machine with:
#   Rust stable, Python 3.12 x64, .NET 10 x64, WebView2 Runtime.
# The script fetches/verifies the pinned sources, prepares the pinned Zig and
# MSYS2 runtime dependencies, checks the Rust workspace, builds the launcher,
# proves its local server starts, and builds the runtime host from source.
# It does not include game files and does not claim a first frame or a race.
param(
    [string]$DataDir = '',
    [int]$Port = 0,
    [switch]$SkipTests,
    [switch]$SkipRuntime
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

function Step([string]$Name) { Write-Host "`n== $Name" }
function Require([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) { throw "Missing prerequisite: $Name" }
}
function Fail([string]$Message) { Write-Host "FAIL: $Message" -ForegroundColor Red; exit 1 }

if ($env:OS -ne 'Windows_NT') { Fail 'Run this script on Windows (the Linux cross-check is documented in docs/en/reports/WINDOWS-2026-11-20.md).' }
Require python
Require cargo
Require rustup

Step 'Inputs and toolchain'
python --version
cargo --version
rustup target list --installed | Out-Null
if (-not $DataDir) { $DataDir = Join-Path $root 'local/windows-verify-data' }
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

Step 'Pinned upstream sources'
python (Join-Path $root 'scripts/fetch-upstream.py')
if ($LASTEXITCODE) { Fail 'fetch-upstream.py' }

Step 'Pinned Zig and MSYS2 runtime dependencies'
python (Join-Path $root 'scripts/setup-native.py')
if ($LASTEXITCODE) { Fail 'setup-native.py (Zig)' }
python (Join-Path $root 'scripts/setup-runtime-deps.py')
if ($LASTEXITCODE) { Fail 'setup-runtime-deps.py (MSYS2 packages; check the hardlink/case fix in scripts/runtime-dlls.py)' }

Step 'Rust checks'
cargo fmt --all -- --check
if ($LASTEXITCODE) { Fail 'cargo fmt --check' }
cargo clippy --workspace --all-targets --locked -- -D warnings
if ($LASTEXITCODE) { Fail 'cargo clippy -D warnings' }
if (-not $SkipTests) {
    cargo test --workspace --locked
    if ($LASTEXITCODE) { Fail 'cargo test' }
}

Step 'Launcher release build'
cargo build --release --locked -p mk8-online
if ($LASTEXITCODE) { Fail 'cargo build -p mk8-online' }
$metadata = cargo metadata --no-deps --format-version 1 | ConvertFrom-Json
$target = $metadata.target_directory
$launcher = Join-Path $target 'release/Launcher.exe'
if (-not (Test-Path -LiteralPath $launcher)) { Fail "Launcher.exe not found at $launcher" }
Get-Item -LiteralPath $launcher | Select-Object FullName, Length
Get-FileHash -Algorithm SHA256 -LiteralPath $launcher | Select-Object Hash

Step 'Launcher headless server'
$out = Join-Path $root 'local/windows-verify-launcher.out'
$err = Join-Path $root 'local/windows-verify-launcher.err'
Remove-Item -LiteralPath $out, $err -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
$process = Start-Process -FilePath $launcher -PassThru -NoNewWindow `
    -ArgumentList @('--headless', '--port', "$Port", '--data-dir', $DataDir) `
    -RedirectStandardOutput $out -RedirectStandardError $err
$url = $null
for ($i = 0; $i -lt 60 -and -not $url; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $out) {
        $match = Select-String -LiteralPath $out -Pattern 'http://127\.0\.0\.1:\d+' -ErrorAction SilentlyContinue
        if ($match) { $url = ($match.Matches[-1].Value) }
    }
    if ($process.HasExited) { break }
}
if (-not $url) {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Get-Content -LiteralPath $out, $err -ErrorAction SilentlyContinue
    Fail 'Launcher did not print its local URL.'
}
Write-Host "Launcher URL: $url"
$response = $null
for ($i = 0; $i -lt 20; $i++) {
    try { $response = Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 5; break }
    catch { Start-Sleep -Milliseconds 500 }
}
if (-not $response -or $response.StatusCode -ne 200) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Fail 'Launcher did not serve HTTP 200.'
}
if ($response.Content -notmatch '<title>MK8-online') {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Fail 'Launcher page does not have the expected title.'
}
Write-Host ("Launcher page: {0} bytes, HTTP {1}" -f $response.Content.Length, $response.StatusCode)
if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }

if (-not $SkipRuntime) {
    Step 'Runtime host from source'
    python (Join-Path $root 'scripts/prebuild-host.py')
    if ($LASTEXITCODE) { Fail 'prebuild-host.py' }
    $hostExe = Join-Path $root 'prebuilt/host/mk8drecomp.exe'
    Get-Item -LiteralPath $hostExe | Select-Object FullName, Length
    Get-FileHash -Algorithm SHA256 -LiteralPath $hostExe | Select-Object Hash

    $exefs = Join-Path $DataDir 'extracted/exefs'
    $gameDll = Join-Path $DataDir 'mk8dgame.dll'
    if ((Test-Path -LiteralPath $exefs) -and (Test-Path -LiteralPath $gameDll)) {
        Step 'Runtime boot log (does not claim a first frame)'
        $log = Join-Path $root 'local/windows-verify-runtime.log'
        $runtime = Start-Process -FilePath $hostExe -PassThru -NoNewWindow `
            -WorkingDirectory $DataDir `
            -ArgumentList @('--play') `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        Start-Sleep -Seconds 20
        if (-not $runtime.HasExited) { Stop-Process -Id $runtime.Id -Force }
        Get-Content -LiteralPath $log, "$log.err" -ErrorAction SilentlyContinue | Select-Object -Last 80
        if (Select-String -LiteralPath $log, "$log.err" -Pattern '\[vk\].*swapchain|\[nvn\] window' -Quiet) {
            Write-Host 'Runtime reached Vulkan/NVN window initialization.' -ForegroundColor Green
        } else {
            Write-Host 'Runtime did not reach Vulkan/NVN initialization; inspect the log above.' -ForegroundColor Yellow
        }
    } else {
        Write-Host "No prepared game at $DataDir (need extracted/exefs and mk8dgame.dll); runtime boot skipped."
    }
}

Write-Host "`nWindows verification completed. This is not gameplay qualification." -ForegroundColor Green
