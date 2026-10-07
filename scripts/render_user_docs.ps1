param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$word = $null
$document = $null
$results = @()
try {
    # Own hidden automation instance. Never attach to or quit the user's Word session.
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.AutomationSecurity = 3
    $word.DisplayAlerts = -1
    foreach ($language in @('RU','EN')) {
        $source = Join-Path $repoRoot "docs\Bot_Training_Studio_User_Guide_$language.docx"
        $before = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        Write-Output "Opening $language guide read-only"
        $document = $word.Documents.Open($source, $false, $true, $false)
        if (!$document.ReadOnly) { throw 'Document did not open read-only' }
        Write-Output "Paginating $language guide"
        $document.Repaginate()
        $pages = $document.ComputeStatistics(2)
        $pdf = Join-Path $Output "$language.pdf"
        Write-Output "Exporting $language guide"
        $document.ExportAsFixedFormat($pdf, 17)
        $document.Close(0)
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($document)
        $document = $null
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $before) { throw 'Word changed the source document' }
        $results += [ordered]@{ language=$language; pages=$pages; readonly=$true; pdf=$pdf; sha256=$before }
    }
    [ordered]@{pass=$true; renderer='Microsoft Word'; guides=$results} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Output 'word-validation.json') -Encoding UTF8
    $results | ConvertTo-Json -Depth 4
} finally {
    if ($null -ne $document) { $document.Close(0); [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($document) }
    if ($null -ne $word) { $word.Quit(0); [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($word) }
}
