# Render Mermaid sources paired with explicit PNG links in docs/*.md.
# Stable filenames are taken from the links; documents are never rewritten.
[CmdletBinding()]
param(
    [string[]] $Docs = @('rtos_design'),
    [switch] $FixLinks,
    [switch] $ValidateOnly,
    [string[]] $OnlyImages = @(),
    [string] $NodePath = 'node',
    [string] $MermaidCliPath,
    [string] $PuppeteerConfig
)
$ErrorActionPreference = 'Stop'
$docsRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$imageRoot = [System.IO.Path]::GetFullPath((Join-Path $docsRoot 'images'))
$imageNamePattern = '^[A-Za-z0-9][A-Za-z0-9_.-]*\.png$'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if ($FixLinks) {
    Write-Warning '-FixLinks is retained for compatibility and validates explicit links only. It never renumbers images or edits Markdown.'
}

# Check every block before rendering, including unselected blocks.
$diagrams = New-Object System.Collections.Generic.List[object]
$seenTargets = @{}
foreach ($doc in $Docs) {
    $docFile = $doc
    if (-not $docFile.EndsWith('.md', [System.StringComparison]::OrdinalIgnoreCase)) {
        $docFile += '.md'
    }
    $markdownPath = [System.IO.Path]::GetFullPath((Join-Path $docsRoot $docFile))
    if ([System.IO.Path]::GetDirectoryName($markdownPath) -ne $docsRoot) {
        throw "Document must be directly inside the script's docs directory: $doc"
    }
    if (-not (Test-Path -LiteralPath $markdownPath -PathType Leaf)) {
        throw "Markdown document not found: $markdownPath"
    }
    $raw = [System.IO.File]::ReadAllText($markdownPath).Replace("`r`n", "`n").Replace("`r", "`n")
    $lines = $raw -split "`n"
    $documentCount = 0
    $inlineCount = 0
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        if ($lines[$lineIndex].Trim() -ne '```mermaid') { continue }
        $sourceLine = $lineIndex + 1
        $imageLine = $lineIndex - 1
        while ($imageLine -ge 0 -and [string]::IsNullOrWhiteSpace($lines[$imageLine])) { $imageLine-- }
        # Foldable source may have one details/summary opening line.
        if ($imageLine -ge 0 -and $lines[$imageLine].Trim() -match '^<details>\s*<summary>.*</summary>\s*$') {
            $imageLine--
            while ($imageLine -ge 0 -and [string]::IsNullOrWhiteSpace($lines[$imageLine])) { $imageLine-- }
        }
        # Explicit native diagrams have no PNG output to generate. Validate their
        # source fence while keeping missing PNG pairings an error elsewhere.
        if ($imageLine -ge 0 -and $lines[$imageLine].Trim() -eq '<!-- diagram:inline -->') {
            $inlineSource = New-Object System.Collections.Generic.List[string]
            $lineIndex++
            while ($lineIndex -lt $lines.Count -and $lines[$lineIndex].Trim() -ne '```') {
                $inlineSource.Add($lines[$lineIndex])
                $lineIndex++
            }
            if ($lineIndex -ge $lines.Count) { throw "${docFile}:$sourceLine Inline Mermaid fence is not closed." }
            if ([string]::IsNullOrWhiteSpace(($inlineSource -join "`n"))) { throw "${docFile}:$sourceLine Inline Mermaid source is empty." }
            $inlineCount++
            continue
        }
        if ($imageLine -lt 0 -or $lines[$imageLine].Trim() -notmatch '^!\[[^\]]*\]\((?<Target>[^)]+)\)$') {
            throw "${docFile}:$sourceLine Mermaid block needs an immediately preceding PNG image link (optional details/summary wrapper allowed)."
        }
        $imageLink = $Matches['Target']
        if ($imageLink -notmatch '^images/(?<File>[A-Za-z0-9][A-Za-z0-9_.-]*\.png)$') {
            throw "${docFile}:$($imageLine + 1) Image target must be images/<filename>.png without subdirectories: $imageLink"
        }
        $imageFile = $Matches['File']
        $imagePath = [System.IO.Path]::GetFullPath((Join-Path $imageRoot $imageFile))
        if ([System.IO.Path]::GetDirectoryName($imagePath) -ne $imageRoot) { throw "Image target escapes docs/images: $imageLink" }
        # Case-insensitive hashtable matches Windows filename identity.
        if ($seenTargets.ContainsKey($imageFile)) {
            throw "Duplicate image target $imageLink in ${docFile}:$sourceLine; already used by $($seenTargets[$imageFile])."
        }
        $seenTargets[$imageFile] = "${docFile}:$sourceLine"
        $source = New-Object System.Collections.Generic.List[string]
        $lineIndex++
        while ($lineIndex -lt $lines.Count -and $lines[$lineIndex].Trim() -ne '```') { $source.Add($lines[$lineIndex]); $lineIndex++ }
        if ($lineIndex -ge $lines.Count) { throw "${docFile}:$sourceLine Mermaid fence is not closed." }
        if ([string]::IsNullOrWhiteSpace(($source -join "`n"))) { throw "${docFile}:$sourceLine Mermaid source is empty." }
        $diagrams.Add([pscustomobject]@{ Document = $docFile; Line = $sourceLine; File = $imageFile; Path = $imagePath; Source = $source -join "`n" })
        $documentCount++
    }
    Write-Host "$docFile : $documentCount paired Mermaid block(s), $inlineCount native block(s)"
}
$selectedNames = @{}
foreach ($imageFile in $OnlyImages) {
    if ($imageFile -notmatch $imageNamePattern) { throw "-OnlyImages accepts output filenames only: $imageFile" }
    if (-not $seenTargets.ContainsKey($imageFile)) { throw "-OnlyImages target has no paired source: $imageFile" }
    if ($selectedNames.ContainsKey($imageFile)) { throw "Duplicate -OnlyImages target: $imageFile" }
    $selectedNames[$imageFile] = $true
}
$selected = @($diagrams | Where-Object { $selectedNames.Count -eq 0 -or $selectedNames.ContainsKey($_.File) })
if ($ValidateOnly) {
    Write-Host "Validated $($diagrams.Count) paired source(s); $($selected.Count) selected. No Markdown or image files changed."
    return
}
if ($selected.Count -eq 0) { Write-Host 'No paired diagrams selected; nothing to render.'; return }

# Use explicitly supplied local tools. Never invoke npx or download packages.
if ([string]::IsNullOrWhiteSpace($MermaidCliPath) -or -not (Test-Path -LiteralPath $MermaidCliPath -PathType Leaf)) {
    throw 'Rendering requires -MermaidCliPath pointing to an installed Mermaid CLI JavaScript entry point.'
}
if ([string]::IsNullOrWhiteSpace($PuppeteerConfig) -or -not (Test-Path -LiteralPath $PuppeteerConfig -PathType Leaf)) {
    throw 'Rendering requires -PuppeteerConfig pointing to a local JSON configuration with executablePath.'
}
$browserConfig = [System.IO.File]::ReadAllText($PuppeteerConfig) | ConvertFrom-Json
if (-not $browserConfig.executablePath -or -not (Test-Path -LiteralPath $browserConfig.executablePath -PathType Leaf)) {
    throw 'Puppeteer configuration executablePath must point to an existing local browser executable.'
}
$nodeExecutable = (Get-Command $NodePath -CommandType Application -ErrorAction Stop).Source
$cliPath = (Resolve-Path -LiteralPath $MermaidCliPath).Path
$configPath = (Resolve-Path -LiteralPath $PuppeteerConfig).Path

function Test-PngHeader([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 8
        if ($stream.Read($header, 0, 8) -ne 8 -or $stream.Length -le 8) { return $false }
        return (($header -join ',') -eq '137,80,78,71,13,10,26,10')
    } finally { $stream.Dispose() }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('rtos-diagrams-' + [guid]::NewGuid().ToString('N'))
$temporaryFiles = New-Object System.Collections.Generic.List[string]
$failures = New-Object System.Collections.Generic.List[string]
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try {
    foreach ($diagram in $selected) {
        $sourcePath = Join-Path $temporaryRoot ($diagram.File + '.mmd')
        $renderPath = Join-Path $temporaryRoot $diagram.File
        $temporaryFiles.Add($sourcePath)
        $temporaryFiles.Add($renderPath)
        [System.IO.File]::WriteAllText($sourcePath, $diagram.Source, $utf8NoBom)
        try {
            $renderOutput = @(& $nodeExecutable $cliPath '-i' $sourcePath '-o' $renderPath '-p' $configPath '-b' 'white' '-s' '2' 2>&1)
            $renderExitCode = $LASTEXITCODE
            if ($renderExitCode -ne 0) { throw "Mermaid CLI exited with code $renderExitCode. $($renderOutput -join [Environment]::NewLine)" }
            if (-not (Test-PngHeader $renderPath)) { throw 'Mermaid CLI did not produce a valid PNG file.' }
            if (-not (Test-Path -LiteralPath $imageRoot -PathType Container)) { New-Item -ItemType Directory -Path $imageRoot | Out-Null }
            # Keep the old file until a successful temporary render is verified.
            Move-Item -LiteralPath $renderPath -Destination $diagram.Path -Force
            $sizeKb = [math]::Round((Get-Item -LiteralPath $diagram.Path).Length / 1KB, 1)
            Write-Host ("OK {0} ({1} KB) <- {2}:{3}" -f $diagram.File, $sizeKb, $diagram.Document, $diagram.Line)
        } catch {
            $failures.Add($diagram.File)
            Write-Warning ("FAILED {0}: {1}" -f $diagram.File, $_.Exception.Message)
        }
    }
} finally {
    # Remove only explicit files created here; never recurse over a computed path.
    foreach ($temporaryFile in $temporaryFiles) {
        if (Test-Path -LiteralPath $temporaryFile -PathType Leaf) { Remove-Item -LiteralPath $temporaryFile }
    }
    Remove-Item -LiteralPath $temporaryRoot
}
if ($failures.Count -gt 0) { throw "$($failures.Count) diagram(s) failed: $($failures -join ', '). Previous images were retained for failed renders." }
Write-Host "Rendered $($selected.Count) diagram(s) -> $imageRoot"
