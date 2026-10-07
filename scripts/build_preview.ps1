param([string]$Output = '', [string]$DocsPython = '', [switch]$DevelopmentOnly)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$projectFile = Join-Path $repoRoot 'src\BotTrainingStudio\BotTrainingStudio.csproj'
$projectVersion = ([xml](Get-Content -LiteralPath $projectFile -Raw)).Project.PropertyGroup.Version
if (!$projectVersion) { throw 'Missing application version' }
if (!$Output) { $Output = Join-Path $repoRoot 'dist\portable-preview-win-x64' }
$Output = [IO.Path]::GetFullPath($Output)
if (!$DevelopmentOnly -and !(Test-Path -LiteralPath (Join-Path $Output 'libraries\studio-libraries.json') -PathType Leaf)) {
    throw 'End-user packages must include libraries. Run scripts/bundle_libraries.py first; -DevelopmentOnly is for incomplete developer builds only.'
}
if (!$DocsPython) {
    $bundledDocsPython = Join-Path $repoRoot 'dist\preview-win-x64\runtime\python.exe'
    if (Test-Path -LiteralPath $bundledDocsPython -PathType Leaf) { $DocsPython = $bundledDocsPython }
    else { $DocsPython = (Get-Command python -ErrorAction Stop).Source }
}
& $DocsPython -I (Join-Path $repoRoot 'scripts\build_user_docs.py') --check
if ($LASTEXITCODE -ne 0) { throw 'Both RU/EN DOCX guides must be rebuilt before packaging' }
# Incremental local build only: no recursive deletion or publication.
& dotnet publish (Join-Path $repoRoot 'src\BotTrainingStudio\BotTrainingStudio.csproj') -c Release -r win-x64 --self-contained true -o $Output --nologo
if ($LASTEXITCODE -ne 0) { throw 'Desktop publish failed' }
foreach ($debugName in @('libHarfBuzzSharp.pdb', 'libSkiaSharp.pdb')) {
    $debugPath = Join-Path $Output $debugName
    if (Test-Path -LiteralPath $debugPath -PathType Leaf) { Remove-Item -LiteralPath $debugPath -Force }
}
foreach ($package in @('opentdm_x_trainer','bts_analysis')) {
    $workerOutput = Join-Path $Output "worker\$package"
    New-Item -ItemType Directory -Force -Path $workerOutput | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $repoRoot "worker\$package") -Filter '*.py' -File | Copy-Item -Destination $workerOutput -Force
}
$nativeOutput = Join-Path $Output 'worker\native'
New-Item -ItemType Directory -Force -Path $nativeOutput | Out-Null
foreach ($file in @('decoder.exe','physics.dll')) {
    $nativeSource = Join-Path $repoRoot "dist\native\$file"
    if (!(Test-Path -LiteralPath $nativeSource -PathType Leaf)) { throw "Build native components first: $file" }
    Copy-Item -LiteralPath $nativeSource -Destination $nativeOutput -Force
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'worker\requirements.txt') -Destination (Join-Path $Output 'worker') -Force
foreach ($file in @('README.md','LICENSE')) { Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $Output -Force }
$docOutput = Join-Path $Output 'docs'
New-Item -ItemType Directory -Force -Path $docOutput | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'docs') -File | Where-Object Extension -in @('.md','.docx','.json') | Copy-Item -Destination $docOutput -Force
$hash = Get-FileHash -LiteralPath (Join-Path $Output 'BotTrainingStudio.exe') -Algorithm SHA256
[ordered]@{ version = $projectVersion; scope = 'development-preview'; exe_sha256 = $hash.Hash; bundled_libraries = (Test-Path -LiteralPath (Join-Path $Output 'libraries\studio-libraries.json')); python_bootstrap = $true; game_installable = $false } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'build.json') -Encoding UTF8
Write-Output (Join-Path $Output 'BotTrainingStudio.exe')
