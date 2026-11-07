$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$toolPython = Join-Path $projectRoot 'local/python/Scripts/python.exe'
if (!(Test-Path -LiteralPath $toolPython)) {
    python -m venv (Join-Path $projectRoot 'local/python')
    if ($LASTEXITCODE -ne 0) { throw 'Falha criando ambiente Python local' }
}
& $toolPython -m pip install --require-hashes -r (Join-Path $projectRoot 'upstream/smm2recomp/recomp/tools/requirements-gen.txt')
if ($LASTEXITCODE -ne 0) { throw 'Falha instalando dependências verificadas do recompilador' }
& $toolPython -m pip install --require-hashes -r (Join-Path $projectRoot 'toolchain/requirements-host.txt')
if ($LASTEXITCODE -ne 0) { throw 'Falha instalando ferramentas do host (Python 3.12 x64)' }
