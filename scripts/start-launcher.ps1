param([int]$Port = 0, [switch]$Headless)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
try {
    # Fontes fixadas do upstream: baixadas e conferidas pelos manifestos (nada é refeito se já baterem).
    python (Join-Path $projectRoot 'scripts/fetch-upstream.py')
    if ($LASTEXITCODE -ne 0) { throw 'Falha obtendo as fontes upstream' }
    cargo build --release --locked -p mk8-online
    if ($LASTEXITCODE -ne 0) { throw 'Falha compilando launcher' }
    $metadata = cargo metadata --no-deps --format-version 1 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'Falha consultando a pasta de build do Cargo' }
    $launcher = Join-Path $metadata.target_directory 'release/Launcher.exe'
    $launcherArgs = @('--port', "$Port")
    if ($Headless) {
        # Sem janela: somente a API e a interface local; endereço em local/launcher.json.
        & $launcher @launcherArgs --headless
    } else {
        Start-Process -FilePath $launcher -ArgumentList $launcherArgs
    }
} finally { Pop-Location }
