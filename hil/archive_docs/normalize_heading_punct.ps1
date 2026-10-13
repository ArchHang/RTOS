# Normalize full-width punctuation in Markdown HEADINGS to half-width + spaced form.
#
# WHY: full-width marks (colon / parens / comma) occupy a full em and sit in the
# VERTICAL CENTRE of the em box, while Latin letters sit ON the baseline. In a
# heading that mixes Latin/Digits with CJK -- e.g.
#   "3.3 pxPortInitialiseStack: ... (CM4F, ... V10.4.6 ...)"
# -- the marks appear to float above the Latin text and insert uneven gaps, so the
# line reads as crooked. At heading size the effect is magnified. Half-width marks
# share the Latin baseline and width, so the line becomes even.
#
# SCOPE: heading lines only ('#'..'######' at line start). Body text is NOT touched;
# Chinese prose conventionally uses full-width punctuation and reads fine at body size.
#
# ENCODING NOTE: this file is deliberately 100% ASCII. Windows PowerShell 5.1 reads
# a BOM-less UTF-8 .ps1 as ANSI, which silently corrupts any literal full-width
# character in the source and makes the -replace pattern match per-character.
# All full-width characters are therefore written as regex \uXXXX escapes.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File normalize_heading_punct.ps1            # dry run
#   powershell -ExecutionPolicy Bypass -File normalize_heading_punct.ps1 -Apply     # write
param(
    [switch] $Apply
)
$ErrorActionPreference = 'Stop'
$ROOT = 'E:\STM32_Project\CubeIDE\f446_rtos\docs'

# detection: any full-width form or CJK punctuation
$FW_ANY = '[\uFF01-\uFF5E\u3000-\u303F]'

function Convert-Heading([string] $h) {
    $s = $h
    $s = $s -replace '\uFF1A', ': '    # full-width colon   ->
    $s = $s -replace '\uFF0C', ', '    # full-width comma   ,
    $s = $s -replace '\uFF08', ' ('    # full-width (        (
    $s = $s -replace '\uFF09', ')'     # full-width )        )
    $s = $s -replace '\u3001', ', '    # ideographic comma   ,
    $s = $s -replace '\uFF1B', '; '    # full-width semicolon
    $s = $s -replace '\uFF1F', '?'     # full-width question
    $s = $s -replace '\uFF01', '!'     # full-width exclamation
    $s = $s -replace '\u3002', '.'     # ideographic full stop
    $s = $s -replace '\u3000', ' '     # ideographic space
    $s = $s -replace '[\u3010\u300A\u3008]', '['   # lenticular / double / single angle open
    $s = $s -replace '[\u3011\u300B\u3009]', ']'   # ... close
    $s = $s -replace '[\u201C\u201D]', '"'         # curly double quotes
    $s = $s -replace '[\u2018\u2019]', "'"         # curly single quotes
    $s = $s -replace '\u2026', '...'               # ellipsis
    $s = $s -replace '\uFF5E', '~'                 # full-width tilde
    # tidy spacing artefacts introduced above
    $s = $s -replace '\s+', ' '
    $s = $s -replace '\s+([\)\]])', '$1'
    $s = $s -replace '([\(\[!])\s+', '$1'
    $s = $s -replace '\[\s*\]', '[]'
    # a closing bracket directly against a word reads cramped: "[x]name" -> "[x] name"
    $s = $s -replace '([\)\]])(?=[A-Za-z0-9`*])', '$1 '
    return $s.Trim()
}

$grandTotal = 0
foreach ($md in Get-ChildItem $ROOT -Filter '*.md' | Sort-Object Name) {
    $orig = [System.IO.File]::ReadAllText($md.FullName)
    $lines = $orig.Replace("`r`n", "`n").Replace("`r", "`n") -split "`n"
    $changed = 0
    $show = @()

    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^#{1,6} ') { continue }
        if ($lines[$i] -notmatch $FW_ANY) { continue }
        $new = Convert-Heading $lines[$i]
        if ($new -ne $lines[$i]) {
            $changed++
            if ($show.Count -lt 3) { $show += [pscustomobject]@{ i = $i + 1; Was = $lines[$i]; Now = $new } }
            $lines[$i] = $new
        }
    }

    if ($changed -eq 0) { continue }
    $grandTotal += $changed
    Write-Host "--- $($md.Name): $changed heading(s) ---"
    foreach ($s in $show) {
        Write-Host "  L$($s.i) OLD: $($s.Was)"
        Write-Host "       NEW: $($s.Now)"
    }
    if ($Apply) {
        [System.IO.File]::WriteAllText($md.FullName, ($lines -join "`n"))
        Write-Host "  -> written"
    }
}

Write-Host ''
Write-Host "headings changed: $grandTotal"

# report anything still containing a full-width / CJK-punctuation mark so that no
# heading silently keeps a character that the rules above do not handle
$left = 0
foreach ($md in Get-ChildItem $ROOT -Filter '*.md' | Sort-Object Name) {
    $raw = [System.IO.File]::ReadAllText($md.FullName)
    $n = 0
    foreach ($l in ($raw.Replace("`r`n", "`n") -split "`n")) {
        if ($l -match '^#{1,6} ' -and $l -match $FW_ANY) {
            $n++
            if ($n -le 3) {
                $codes = ([regex]::Matches($l, $FW_ANY) | ForEach-Object { 'U+{0:X4}' -f [int][char]$_.Value }) -join ' '
                Write-Host "  UNHANDLED $($md.Name): $codes"
                Write-Host "            $l"
            }
        }
    }
    $left += $n
}
Write-Host "headings still containing full-width marks: $left"
if (-not $Apply) { Write-Host 'DRY RUN -- re-run with -Apply to write' }
