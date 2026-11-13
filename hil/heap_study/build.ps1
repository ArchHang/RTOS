# Build an isolated F446 image for the allocator business and TIM5 fixtures.
#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('current', 'zephyr')][string] $Backend,
    [ValidateSet('industry', 'irq', 'irq_direct', 'periodic', 'periodic_direct')]
    [string] $Phase = 'industry',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$')][string] $Tag,
    [string] $ZephyrSource,
    [string] $ToolchainDirectory,
    [string] $MakeExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$backendName = $Backend.ToLowerInvariant()
$phaseName = $Phase.ToLowerInvariant()
if ($ZephyrSource -and $backendName -ne 'zephyr') {
    throw '-ZephyrSource requires -Backend zephyr.'
}
if (-not $Tag) {
    $Tag = "${phaseName}_${backendName}_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$runRoot = Join-Path $root "hil/logs/heap_study/$Tag"
if (Test-Path -LiteralPath $runRoot) {
    throw "Build directory already exists: $runRoot"
}
$project = Join-Path $runRoot 'project'
$projectHil = Join-Path $project 'hil'
New-Item -ItemType Directory -Path $projectHil | Out-Null

foreach ($directory in 'Core', 'Drivers', 'AI', 'RTOS') {
    Copy-Item -LiteralPath (Join-Path $root $directory) -Destination $project -Recurse
}
Get-ChildItem -LiteralPath $root -File -Filter '*.ld' | Copy-Item -Destination $project
foreach ($entry in 'build_fw.ps1', 'build_layout.ps1', 'templates', 'fw') {
    Copy-Item -LiteralPath (Join-Path $root "hil/$entry") -Destination $projectHil -Recurse
}

# Optional tool overrides apply only to the isolated copy of the standard HIL builder.
$builder = Join-Path $projectHil 'build_fw.ps1'
if ($ToolchainDirectory -or $MakeExecutable) {
    $builderSource = Get-Content -LiteralPath $builder -Raw
    $toolPaths = [ordered]@{ TOOLS = $ToolchainDirectory; MAKE = $MakeExecutable }
    foreach ($entry in $toolPaths.GetEnumerator()) {
        if (-not $entry.Value) { continue }
        $path = (Resolve-Path -LiteralPath $entry.Value).Path.Replace("'", "''")
        $line = '(?m)^\$' + $entry.Key + "\s*= '[^'\r\n]*'"
        if ([regex]::Matches($builderSource, $line).Count -ne 1) {
            throw "HIL builder assignment for $($entry.Key) changed."
        }
        $builderSource = [regex]::Replace($builderSource, $line, {
            '$' + $entry.Key + " = '$path'"
        })
    }
    Set-Content -LiteralPath $builder -Value $builderSource -NoNewline
}

$memory = Join-Path $project 'RTOS/Memory'
$backendDirectory = Join-Path $memory 'industry_backend'
New-Item -ItemType Directory -Path $backendDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'backend/adapter.h') -Destination $backendDirectory

$kernelHeap = Join-Path $memory 'rtos_heap.c'
$upstreamHashes = [ordered]@{}
if ($backendName -eq 'current') {
    Copy-Item -LiteralPath $kernelHeap -Destination (Join-Path $backendDirectory 'heap.c')
} else {
    $zephyrRoot = Join-Path $PSScriptRoot 'backend/zephyr'
    Copy-Item -LiteralPath (Join-Path $zephyrRoot 'include') -Destination $backendDirectory -Recurse
    $upstream = Join-Path $backendDirectory 'upstream'
    New-Item -ItemType Directory -Path $upstream | Out-Null
    $origin = 'https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/v4.2.0'
    $pinnedSources = [ordered]@{
        'lib/heap/heap.c' = '2a939f2544eeafbe4cf3d77b756cb602917a3fd3cfc5aa5ec29e85561ee4644e'
        'lib/heap/heap.h' = 'ac025cc83f60b2a6859bf0c1ad5f63a10e5428f38b3625517dcca70cb0244bb9'
        'LICENSE' = 'c6596eb7be8581c18be736c846fb9173b69eccf6ef94c5135893ec56bd92ba08'
    }
    foreach ($source in $pinnedSources.GetEnumerator()) {
        $name = Split-Path -Leaf $source.Key
        $destination = Join-Path $upstream $name
        if ($ZephyrSource) {
            Copy-Item -LiteralPath (Join-Path $ZephyrSource $name) -Destination $destination
        } else {
            Invoke-WebRequest -Uri "$origin/$($source.Key)" -OutFile $destination -TimeoutSec 30
        }
        $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $source.Value) {
            throw "SHA-256 mismatch for Zephyr v4.2.0 $($source.Key): $actual"
        }
        $upstreamHashes[$source.Key] = $actual
    }
}

$direct = $phaseName -in 'irq_direct', 'periodic_direct'
$periodic = $phaseName -in 'periodic', 'periodic_direct'
$fixtureName = if ($phaseName -in 'irq', 'irq_direct') { 'f446_heap_irq_latency' } else { 'f446_heap_industry' }
$defines = @("INDUSTRY_BACKEND=$backendName")
$adapterSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot "backend/$backendName.c") -Raw
if ($direct) {
    $defines += 'HEAP_IRQ_INSTRUMENTED=1'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'backend/measure_mask.h') -Destination $backendDirectory
    $adapterSource = '#include "industry_backend/measure_mask.h"' + "`n" + $adapterSource
}
Set-Content -LiteralPath (Join-Path $memory 'industry_adapter.c') -Value $adapterSource -NoNewline

$rule = Join-Path $projectHil 'templates/RTOS_Memory.mk'
$makeSource = Get-Content -LiteralPath $rule -Raw
$makeSections = [ordered]@{
    C_SRCS = '../RTOS/Memory/industry_adapter.c'
    OBJS = './RTOS/Memory/industry_adapter.o'
    C_DEPS = './RTOS/Memory/industry_adapter.d'
}
foreach ($section in $makeSections.GetEnumerator()) {
    $marker = $section.Key + ' += ' + [char]92
    if (-not $makeSource.Contains($marker)) {
        throw "Missing $($section.Key) in the RTOS Memory make template."
    }
    $makeSource = $makeSource.Replace($marker, $marker + "`n" + $section.Value + ' ' + [char]92)
}
if ($backendName -eq 'zephyr') {
    $makeSource = $makeSource.Replace('arm-none-eabi-gcc ', 'arm-none-eabi-gcc -I../RTOS/Memory/industry_backend/include ')
}
Set-Content -LiteralPath $rule -Value $makeSource -NoNewline
Add-Content -LiteralPath (Join-Path $projectHil 'templates/objects.list.template') -Value './RTOS/Memory/industry_adapter.o'

$fixture = Join-Path $projectHil "fw/Src/$fixtureName.c"
if ($periodic) {
    $defines += 'HEAP_IRQ_BACKGROUND=1'
    $timer = Get-Content -LiteralPath (Join-Path $projectHil 'fw/Src/f446_heap_irq_latency.c') -Raw
    $business = Get-Content -LiteralPath $fixture -Raw
    $fixture = Join-Path $runRoot 'industry_periodic.c'
    # Both sources have an internal add_sample(). Preserve the original calls via a scoped rename.
    $combined = "#define add_sample add_irq_sample`n$timer`n#undef add_sample`n$business"
    Set-Content -LiteralPath $fixture -Value $combined -NoNewline
}

$buildArguments = @('-Variant', 'portable_fixture', '-FixtureSource', $fixture,
    '-FixtureEntry', "${fixtureName}_run", '-FixturePriority', '4', '-Optimization', 'O2',
    '-CriticalMask', 'basepri', '-FixtureDefines', ($defines -join ','))
$buildLog = Join-Path $runRoot 'build.txt'
& (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File $builder @buildArguments 2>&1 |
    Set-Content -LiteralPath $buildLog
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $buildLog -Tail 15 | Write-Output
    throw "F446 build failed. See $buildLog."
}

$fixtureHash = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
$payload = Join-Path $projectHil 'fwsrc/portable_payload.c'
if ($fixtureHash -ne (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash) {
    throw 'Compiled fixture does not match the requested input.'
}
$elf = Join-Path $project 'Debug/f446_rtos.elf'
$map = Join-Path $project 'Debug/f446_rtos.map'
$manifest = [ordered]@{
    backend = $backendName
    phase = $phaseName
    defines = $defines
    optimization = 'O2'
    critical_mask = 'basepri'
    arena_bytes = 32768
    fixture_sha256 = $fixtureHash
    kernel_heap_sha256 = (Get-FileHash -LiteralPath $kernelHeap -Algorithm SHA256).Hash
    adapter_sha256 = (Get-FileHash -LiteralPath (Join-Path $memory 'industry_adapter.c') -Algorithm SHA256).Hash
    upstream_sha256 = $upstreamHashes
    elf = 'project/Debug/f446_rtos.elf'
    map = 'project/Debug/f446_rtos.map'
    elf_sha256 = (Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $runRoot 'source_identity.json')
Write-Output "ELF: $elf"
Write-Output "Map: $map"
Write-Output "Build log: $buildLog"
