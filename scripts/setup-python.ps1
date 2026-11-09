function Get-PythonArchiveHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hash.Dispose() }
}

# Official CPython NuGet distribution: full stdlib, venv and pip, no system installation.
function Assert-PythonDirectory([string]$Path, [string]$Root) {
    $full = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (!$full.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw 'Python path outside local directory' }
    $current = Get-Item -LiteralPath $Path -Force
    while ($current) {
        if ($current.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Python directory contains a junction or symbolic link' }
        $current = $current.Parent
    }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($full)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Python directory contains a junction or symbolic link' }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
    foreach ($process in Get-Process python, pythonw -ErrorAction SilentlyContinue) {
        if ($process.Path -and $process.Path.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Python environment is in use; retry after the current task finishes / Ambiente Python em uso'
        }
    }
}

function Install-PrivatePython([string]$Local) {
    $version = '3.12.10'
    $sha256 = '0eb85c2dfccccf1b17352de4c397f69194035b7d37149eacc16f1147d93de3b8'
    $pythonRoot = Join-Path $Local "cpython-$version"
    $exe = Join-Path $pythonRoot 'tools/python.exe'
    New-Item -ItemType Directory -Force -Path $Local | Out-Null
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Local)))
    if ($drive.AvailableFreeSpace -lt 4GB + 250MB) { throw 'Python setup requires 250 MiB plus the 4 GiB disk reserve' }
    if (Test-ToolPython $exe) { return $exe }
    $archive = Join-Path $Local "python-$version.zip"
    if (!(Test-Path -LiteralPath $archive) -or (Get-PythonArchiveHash $archive) -ne $sha256) {
        Write-Host 'Downloading Python 3.12 x64 / Baixando Python 3.12 x64 (14 MB)'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $partial = Join-Path $Local ('python-download-' + [guid]::NewGuid().ToString('N') + '.zip')
        Invoke-WebRequest -UseBasicParsing -Uri "https://api.nuget.org/v3-flatcontainer/python/$version/python.$version.nupkg" -OutFile $partial
        if ((Get-PythonArchiveHash $partial) -ne $sha256) { throw 'Python download SHA-256 mismatch; refusing installation' }
        Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    if (Test-Path -LiteralPath $pythonRoot) {
        Assert-PythonDirectory $pythonRoot $Local
        Rename-Item -LiteralPath $pythonRoot -NewName ("cpython-$version-backup-" + [guid]::NewGuid().ToString('N'))
    }
    Write-Host 'Extracting verified Python / Extraindo Python verificado'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $pythonRoot)
    if (!(Test-ToolPython $exe)) { throw 'Downloaded Python failed validation / Python baixado invalido' }
    return $exe
}
