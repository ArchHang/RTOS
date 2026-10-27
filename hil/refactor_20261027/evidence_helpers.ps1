# Read-only source identity snapshot shared by this refactor's new test batch.
function Get-RefactorSourceManifest {
    param([string] $ProjectRoot)
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($relative in @('RTOS','Core','hil/fw')) {
        Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $relative) -Recurse -File |
            Where-Object { $_.Extension -in @('.c','.h','.s','.S') } |
            ForEach-Object { $paths.Add($_.FullName) }
    }
    Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'hil/templates') -File |
        Where-Object { $_.Extension -in @('.mk','.template') } |
        ForEach-Object { $paths.Add($_.FullName) }
    foreach ($relative in @('hil/build_fw.ps1','hil/build_layout.ps1',
                            'hil/run_hil.ps1','hil/restore_project.ps1','hil/repro_all.ps1','.cproject')) {
        $paths.Add((Join-Path $ProjectRoot $relative))
    }
    Get-ChildItem -LiteralPath $PSScriptRoot -File |
        Where-Object { $_.Extension -eq '.ps1' } |
        ForEach-Object { $paths.Add($_.FullName) }
    Get-ChildItem -LiteralPath $ProjectRoot -File -Filter '*.ld' |
        ForEach-Object { $paths.Add($_.FullName) }
    return @($paths | Sort-Object -Unique | ForEach-Object {
        [ordered]@{path=$_.Substring($ProjectRoot.Length + 1).Replace('\','/');
            sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}
    })
}
