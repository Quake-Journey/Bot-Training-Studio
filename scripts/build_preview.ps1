param([string]$Output = '')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (!$Output) { $Output = Join-Path $repoRoot 'dist\preview-win-x64' }
$Output = [IO.Path]::GetFullPath($Output)
# Incremental local build only: no recursive deletion or publication.
& dotnet publish (Join-Path $repoRoot 'src\BotTrainingStudio\BotTrainingStudio.csproj') -c Release -r win-x64 --self-contained true -o $Output --nologo
if ($LASTEXITCODE -ne 0) { throw 'Desktop publish failed' }
$workerOutput = Join-Path $Output 'worker\opentdm_x_trainer'
New-Item -ItemType Directory -Force -Path $workerOutput | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'worker\opentdm_x_trainer') -Filter '*.py' -File | Copy-Item -Destination $workerOutput -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'worker\requirements.txt') -Destination (Join-Path $Output 'worker') -Force
foreach ($file in @('README.md','LICENSE')) { Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $Output -Force }
$docOutput = Join-Path $Output 'docs'
New-Item -ItemType Directory -Force -Path $docOutput | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'docs') -Filter '*.md' -File | Copy-Item -Destination $docOutput -Force
$hash = Get-FileHash -LiteralPath (Join-Path $Output 'BotTrainingStudio.exe') -Algorithm SHA256
[ordered]@{ version = '0.1.0-preview'; scope = 'local-development'; exe_sha256 = $hash.Hash; bundled_ml_runtime = $false; game_installable = $false } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'build.json') -Encoding UTF8
Write-Output (Join-Path $Output 'BotTrainingStudio.exe')
