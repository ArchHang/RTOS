# Shared source-directory layout for HIL builds and daily restoration.
# Dot-source this file; it never builds, flashes, opens a port, or edits sources.

function Get-HilBuildRules {
    $directories = @(
        'AI/output/int8',
        'Core/Src',
        'Core/Startup',
        'Drivers/STM32F4xx_HAL_Driver/Src',
        'RTOS/Core/Src',
        'RTOS/IPC',
        'RTOS/Memory',
        'RTOS/Services',
        'RTOS/USB',
        'RTOS/Port/GCC/ARM_CM4F_CM7F',
        'RTOS/lib/third/CherryUSB/class/cdc',
        'RTOS/lib/third/CherryUSB/class/hid',
        'RTOS/lib/third/CherryUSB/class/msc',
        'RTOS/lib/third/CherryUSB/core',
        'RTOS/lib/third/CherryUSB/port/dwc2'
    )
    foreach ($relative in $directories) {
        $template = if ($relative -eq 'Core/Src') {
            'subdir.mk.template'
        } else {
            ($relative -replace '/', '_') + '.mk'
        }
        [pscustomobject]@{ Relative = $relative; Template = $template }
    }
}

function Assert-HilBuildPath {
    param([string] $ProjectRoot, [string] $DebugRoot, [string] $Target)
    $rootPrefix = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\') + '\'
    $debugPath = [IO.Path]::GetFullPath($DebugRoot).TrimEnd('\')
    $targetPath = [IO.Path]::GetFullPath($Target)
    if (-not $debugPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($debugPath) -ne 'Debug' -or
        -not $targetPath.StartsWith($debugPath + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Build path escapes the project's Debug directory: $Target"
    }
}

function Set-HilBuildRule {
    param(
        [string] $ProjectRoot,
        [string] $DebugRoot,
        [string] $Relative,
        [string] $Content
    )
    $rulePath = Join-Path $DebugRoot "$Relative/subdir.mk"
    Assert-HilBuildPath -ProjectRoot $ProjectRoot -DebugRoot $DebugRoot -Target $rulePath
    $previous = if (Test-Path -LiteralPath $rulePath) {
        [IO.File]::ReadAllText($rulePath)
    } else { $null }
    if ($previous -cne $Content) {
        $directory = Split-Path $rulePath -Parent
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        [IO.File]::WriteAllText($rulePath, $Content)
        # Make does not track compiler-flag changes. Invalidate only this rule's
        # direct outputs; source directories and historical evidence are separate.
        Get-ChildItem -LiteralPath $directory -File |
            Where-Object { $_.Extension -in @('.o', '.d', '.su', '.cyclo') } |
            ForEach-Object {
                Assert-HilBuildPath -ProjectRoot $ProjectRoot -DebugRoot $DebugRoot `
                    -Target $_.FullName
                Remove-Item -LiteralPath $_.FullName -Force
            }
    }
}

function Initialize-HilBuildTree {
    param([string] $ProjectRoot, [string] $DebugRoot, [switch] $RestoreRules)
    $templates = Join-Path $ProjectRoot 'hil/templates'
    foreach ($rule in @(Get-HilBuildRules)) {
        $source = Join-Path $templates $rule.Template
        $target = Join-Path $DebugRoot "$($rule.Relative)/subdir.mk"
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing canonical build template: $source"
        }
        if ($RestoreRules -or -not (Test-Path -LiteralPath $target)) {
            Set-HilBuildRule -ProjectRoot $ProjectRoot -DebugRoot $DebugRoot `
                -Relative $rule.Relative -Content ([IO.File]::ReadAllText($source))
        }
    }
    foreach ($name in @('makefile', 'sources.mk', 'objects.mk', 'objects.list')) {
        $source = Join-Path $templates "$name.template"
        $target = Join-Path $DebugRoot $name
        Assert-HilBuildPath -ProjectRoot $ProjectRoot -DebugRoot $DebugRoot -Target $target
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing canonical build template: $source"
        }
        # Always refresh the directory inventory. The build script writes its
        # variant link command and object list after this initialization.
        if ($RestoreRules -or $name -in @('sources.mk', 'objects.mk') -or
            -not (Test-Path -LiteralPath $target)) {
            Copy-Item -LiteralPath $source -Destination $target -Force
        }
    }
}
