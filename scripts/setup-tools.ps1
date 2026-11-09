$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$env:PIP_CONFIG_FILE = 'NUL' # Ignore user/system pip.ini; use only the pinned binary packages.
$toolPython = Join-Path $projectRoot 'local/python/Scripts/python.exe'

function Test-ToolPython([string]$Program, [string[]]$Prefix = @()) {
    try {
        & $Program @Prefix -I -c 'import sys, pip; sys.exit(0 if sys.version_info[:2] == (3, 12) and sys.maxsize > 2**32 else 1)' 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } catch { return $false }
}

if (!(Test-ToolPython $toolPython)) {
    . (Join-Path $PSScriptRoot 'setup-python.ps1')
    $python = Install-PrivatePython (Join-Path $projectRoot 'local')
    $environment = Join-Path $projectRoot 'local/python'
    if (Test-Path -LiteralPath $environment) {
        Assert-PythonDirectory $environment (Join-Path $projectRoot 'local')
        $backup = 'python-backup-' + [guid]::NewGuid().ToString('N')
        Rename-Item -LiteralPath $environment -NewName $backup
        Write-Output "Previous Python environment preserved / Ambiente anterior preservado: $backup"
    }
    Write-Output 'Configuring Python 3.12 x64 / Configurando Python 3.12 x64'
    & $python -I -m venv $environment
    if ($LASTEXITCODE -ne 0 -or !(Test-ToolPython $toolPython)) {
        throw 'Failed to create Python 3.12 environment / Falha criando ambiente Python 3.12'
    }
}
& $toolPython -I -m pip --isolated --disable-pip-version-check install --only-binary=:all: --index-url https://pypi.org/simple --require-hashes -r (Join-Path $projectRoot 'upstream/smm2recomp/recomp/tools/requirements-gen.txt')
if ($LASTEXITCODE -ne 0) { throw 'Falha instalando dependências verificadas do recompilador' }
& $toolPython -I -m pip --isolated --disable-pip-version-check install --only-binary=:all: --index-url https://pypi.org/simple --require-hashes -r (Join-Path $projectRoot 'toolchain/requirements-host.txt')
if ($LASTEXITCODE -ne 0) { throw 'Falha instalando ferramentas do host (Python 3.12 x64)' }
