# Convert image references in a Markdown doc between LINKED and EMBEDDED (data URI) form.
#
# USE CASE: a document that must be carried to another machine as a SINGLE file.
# Normally images are linked (`![](images/x.png)`), which is better for editing and
# for version control (a picture change is a one-file change, and the .md diff stays
# readable). But a linked doc is not self-contained: copy the .md alone and the
# pictures vanish.
#
# RECOMMENDED WORKFLOW (keep the repo clean):
#   * SOURCE OF TRUTH  = <doc>.md          (linked; what you edit and commit)
#   * DISTRIBUTION     = <doc>.embed.md    (self-contained; generated, git-ignore it)
# Edit the linked file, then re-run -Embed whenever you need to move the doc.
# Do NOT edit the .embed.md by hand -- your changes will be overwritten.
#
# NOTE ON QUALITY: data URIs are lossless base64. The picture is pixel-identical to
# the linked original; only the transport encoding differs. Cost is ~+33% file size.
#
# Usage:
#   # make a self-contained copy for carrying to another machine
#   powershell -ExecutionPolicy Bypass -File toggle_embed_images.ps1 -Doc rtos_design -Embed
#
#   # convert an .embed.md back into a normal doc (writes the PNGs out again)
#   powershell -ExecutionPolicy Bypass -File toggle_embed_images.ps1 -Doc rtos_design.embed -Extract
#
#   # report status only
#   powershell -ExecutionPolicy Bypass -File toggle_embed_images.ps1 -Doc rtos_design
param(
    [Parameter(Mandatory = $true)][string] $Doc,
    [switch] $Embed,
    [switch] $Extract,
    # extraction destination, relative to docs/ (default images/). Use a separate
    # folder to round-trip-verify without overwriting the originals.
    [string] $OutDir = 'images'
)
$ErrorActionPreference = 'Stop'
$ROOT = 'E:\STM32_Project\CubeIDE\f446_rtos\docs'

$src = Join-Path $ROOT "$Doc.md"
if (-not (Test-Path $src)) { throw "missing: $src" }
$text = [System.IO.File]::ReadAllText($src)

# ![alt](target "optional title")
$IMG_RE = '!\[(?<alt>[^\]]*)\]\((?<target>[^)\s]+)(?<title>\s+"[^"]*")?\)'

# ---------------------------------------------------------------- EMBED
if ($Embed) {
    $dst = Join-Path $ROOT "$Doc.embed.md"
    $missing = New-Object System.Collections.Generic.List[string]
    $done = 0
    $bytes = 0

    $result = [regex]::Replace($text, $IMG_RE, {
        param($m)
        $target = $m.Groups['target'].Value
        if ($target.StartsWith('data:')) { return $m.Value }        # already embedded
        if ($target -match '^https?:') { return $m.Value }          # remote: leave alone

        $path = Join-Path $ROOT ($target -replace '/', '\')
        if (-not (Test-Path $path)) { $missing.Add($target); return $m.Value }

        $ext = [System.IO.Path]::GetExtension($path).ToLower()
        $mime = switch ($ext) {
            '.png'  { 'image/png' }
            '.jpg'  { 'image/jpeg' }
            '.jpeg' { 'image/jpeg' }
            '.gif'  { 'image/gif' }
            '.svg'  { 'image/svg+xml' }
            '.webp' { 'image/webp' }
            default { 'application/octet-stream' }
        }
        $b = [System.IO.File]::ReadAllBytes($path)
        $script:bytes += $b.Length
        $script:done++
        $b64 = [System.Convert]::ToBase64String($b)
        # keep the original title, if any
        return '![' + $m.Groups['alt'].Value + '](data:' + $mime + ';base64,' + $b64 + ')' +
               $m.Groups['title'].Value
    })

    foreach ($m in $missing) { Write-Host "  WARN missing image: $m" }
    [System.IO.File]::WriteAllText($dst, $result)

    $srcKB = [math]::Round((Get-Item $src).Length / 1KB, 1)
    $dstKB = [math]::Round((Get-Item $dst).Length / 1KB, 1)
    Write-Host "embedded $done image(s), $([math]::Round($bytes/1KB,1)) KB of PNG"
    Write-Host "  $Doc.md        -> $srcKB KB (linked, source of truth)"
    Write-Host "  $Doc.embed.md  -> $dstKB KB (self-contained, carry this one)"
    Write-Host "  carry this single file to another machine; no images/ folder needed."
    return
}

# -------------------------------------------------------------- EXTRACT
if ($Extract) {
    $imgDir = Join-Path $ROOT $OutDir
    New-Item -ItemType Directory -Force -Path $imgDir | Out-Null
    $n = 0
    $extMap = @{ 'image/png' = '.png'; 'image/jpeg' = '.jpg'; 'image/gif' = '.gif';
                 'image/svg+xml' = '.svg'; 'image/webp' = '.webp' }

    $result = [regex]::Replace($text, $IMG_RE, {
        param($m)
        $target = $m.Groups['target'].Value
        if (-not $target.StartsWith('data:')) { return $m.Value }
        if ($target -notmatch '^data:(?<mime>[^;]+);base64,(?<b64>.+)$') {
            Write-Host "  WARN unrecognised data URI"; return $m.Value
        }
        $script:n++
        $mime = $Matches['mime']
        $ext = if ($extMap.ContainsKey($mime)) { $extMap[$mime] } else { '.bin' }
        $name = '{0}_{1:D2}{2}' -f ($Doc -replace '\.embed$',''), $script:n, $ext
        $out = Join-Path $imgDir $name
        [System.IO.File]::WriteAllBytes($out, [System.Convert]::FromBase64String($Matches['b64']))
        return '![' + $m.Groups['alt'].Value + '](' + $OutDir + '/' + $name + ')' + $m.Groups['title'].Value
    })

    $dst = Join-Path $ROOT "$($Doc -replace '\.embed$','').extracted.md"
    [System.IO.File]::WriteAllText($dst, $result)
    Write-Host "extracted $n image(s) to $OutDir/, doc written to $([System.IO.Path]::GetFileName($dst))"
    return
}

# --------------------------------------------------------------- STATUS
$linked  = ([regex]::Matches($text, '!\[[^\]]*\]\((?!data:)[^)\s]+\)')).Count
$embedded = ([regex]::Matches($text, '!\[[^\]]*\]\(data:')).Count
Write-Host "$Doc.md : linked=$linked  embedded=$embedded"
Write-Host "  use -Embed to make a single-file copy, -Extract to unpack one."
