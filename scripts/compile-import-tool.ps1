param([string]$Source, [string]$Runtime, [string]$LibHac, [string]$Output)
$ErrorActionPreference = 'Stop'
$refs = @()
foreach ($file in Get-ChildItem -LiteralPath $Runtime -Filter '*.dll') {
    try { [void][Reflection.AssemblyName]::GetAssemblyName($file.FullName) } catch { continue }
    $refs += [Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile($file.FullName)
}
$refs += [Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile($LibHac)
$tree = [Microsoft.CodeAnalysis.CSharp.CSharpSyntaxTree]::ParseText((Get-Content -LiteralPath $Source -Raw))
$options = [Microsoft.CodeAnalysis.CSharp.CSharpCompilationOptions]::new([Microsoft.CodeAnalysis.OutputKind]::ConsoleApplication)
$compilation = [Microsoft.CodeAnalysis.CSharp.CSharpCompilation]::Create(
    'mk8-import', [Microsoft.CodeAnalysis.SyntaxTree[]]@($tree), [Microsoft.CodeAnalysis.MetadataReference[]]$refs, $options)
$stream = [IO.File]::Create($Output)
try { $result = $compilation.Emit($stream) } finally { $stream.Dispose() }
if (!$result.Success) {
    $result.Diagnostics | ForEach-Object { Write-Error $_.ToString() -ErrorAction Continue }
    exit 1
}
