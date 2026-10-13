# Render every ```mermaid block embedded in a Markdown document into a PNG.
#
# ---------------------------------------------------------------------------
# 为什么把图源放在 .md 里, 而不是单独的 .mmd 文件?
#   .md 是唯一的真源(single source of truth): 改图只需改 .md 里的代码块,
#   不会出现"文档改了、.mmd 没改"或两者不一致的问题。
#   代价是渲染时需要本脚本从 .md 中抽取代码块(而不是直接读 .mmd)。
#
# 为什么要生成 PNG 而不是只留 mermaid 代码块?
#   本项目文档的读者可能使用 STM32CubeIDE / Word / 纯文本编辑器查看,
#   这些环境不渲染 mermaid。PNG 在任何 Markdown 查看器中都能显示。
#   因此每张图同时保留: 可见的 PNG + 紧随其后的 mermaid 源码块。
#
# 生成的 PNG 是构建产物, 不是手写文件 —— 修改图后请重新运行本脚本。
# ---------------------------------------------------------------------------
#
# Design: the mermaid source lives ONLY inside the document. Each block is
# numbered in document order and rendered to docs/images/<doc>_<NN>.png.
# The document refers to it with a normal Markdown image link placed
# immediately BEFORE the block:
#
#     ![caption](images/rtos_design_03.png)
#
#     ```mermaid
#     ...diagram source...
#     ```
#
# The script verifies that each block has a matching image link with the
# expected number, so drift between text and image is reported instead of
# passing silently.
#
# Dependency: mermaid-cli is fetched on demand via npx (needs Node.js + network
# on first run, then it is cached in the npm cache).
#
# Usage: powershell -ExecutionPolicy Bypass -Command "& .\gen_diagrams.ps1 -Docs rtos_design,rtos_performance_report"
param(
    [string[]] $Docs = @('rtos_design'),
    [switch]   $FixLinks
)
$ErrorActionPreference = 'Stop'

$ROOT = 'E:\STM32_Project\CubeIDE\f446_rtos\docs'
$IMG = Join-Path $ROOT 'images'
$TMP = Join-Path $env:TEMP 'mmd_work'
New-Item -ItemType Directory -Force -Path $IMG, $TMP | Out-Null

$totalFail = 0

foreach ($doc in $Docs) {
    $MD = Join-Path $ROOT "$doc.md"
    if (-not (Test-Path $MD)) { Write-Host "SKIP (missing): $MD"; continue }

    # Normalize line endings: a stray CR left by splitting on LF only would merge
    # two mermaid lines into one and produce bogus parse errors.
    $raw = [System.IO.File]::ReadAllText($MD).Replace("`r`n", "`n").Replace("`r", "`n")
    $lines = $raw -split "`n"

    # ---- collect mermaid blocks in document order ----
    $blocks = New-Object System.Collections.Generic.List[string]
    $buf = $null
    foreach ($line in $lines) {
        if ($buf -eq $null) {
            if ($line.Trim() -eq '```mermaid') { $buf = New-Object System.Collections.Generic.List[string] }
            continue
        }
        if ($line.Trim() -eq '```') { $blocks.Add(($buf -join "`n")); $buf = $null; continue }
        $buf.Add($line)
    }

    if ($blocks.Count -eq 0) { Write-Host "$doc : no mermaid blocks"; continue }
    Write-Host "$doc : $($blocks.Count) mermaid block(s)"

    # ---- 校验并(可选)自动修正图片链接编号 ----
    # 在文档中间插入新图会使后续图片整体错位; -FixLinks 把每个 mermaid 块**紧邻
    # 之前**的图片链接改写成该块应有的编号, 省去手工重编号。
    $fixed = 0
    $lastImgLine = -1
    $lastImgAlt  = ''
    $blockIdx = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '!\[([^\]]*)\]\(images/[^)]+\.png\)') {
            $lastImgLine = $i
            $lastImgAlt  = $Matches[1]
            continue
        }
        if ($lines[$i].Trim() -eq '```mermaid') {
            $blockIdx++
            $want = "{0}_{1:D2}.png" -f $doc, $blockIdx
            if ($lastImgLine -ge 0) {
                if ($lines[$lastImgLine] -notmatch [regex]::Escape($want)) {
                    if ($FixLinks) {
                        $lines[$lastImgLine] = "![$lastImgAlt](images/$want)"
                        $fixed++
                        Write-Host "  fix  block #$blockIdx -> images/$want"
                    } else {
                        Write-Host "  WARN: block #$blockIdx expects images/$want but link says otherwise (use -FixLinks)"
                    }
                }
            } else {
                Write-Host "  WARN: mermaid block #$blockIdx has no preceding image link"
            }
        }
    }
    if ($fixed -gt 0) {
        [System.IO.File]::WriteAllText($MD, ($lines -join "`n"))
        Write-Host "  -> rewrote $fixed image link(s) in $doc.md"
        $raw = ($lines -join "`n")
    }

    # ---- render ----
    for ($i = 0; $i -lt $blocks.Count; $i++) {
        $name = "{0}_{1:D2}" -f $doc, ($i + 1)
        $mmd = Join-Path $TMP "$name.mmd"
        $png = Join-Path $IMG "$name.png"
        [System.IO.File]::WriteAllText($mmd, $blocks[$i])

        $out = & cmd /c "npx -y @mermaid-js/mermaid-cli@10.9.1 -i `"$mmd`" -o `"$png`" -b white -s 2 2>&1"
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $png)) {
            Write-Host "  FAILED: $name"
            $out | Select-String -Pattern 'Error|error|Parse' | Select-Object -First 3 | ForEach-Object { Write-Host "     $_" }
            $totalFail++
        } else {
            $kb = [math]::Round((Get-Item $png).Length / 1KB, 1)
            Write-Host ("  OK  {0,-40} {1,7} KB" -f "$name.png", $kb)
        }
    }
}

Write-Host ""
if ($totalFail -gt 0) { Write-Host "$totalFail diagram(s) failed"; exit 1 }
Write-Host "all diagrams OK -> $IMG"
