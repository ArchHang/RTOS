# Read-only evidence validation; never builds, flashes, resets, or opens COM3.
# Default mode requires every formal case and the final daily restoration.
# -InspectTag parses one known stress capture and prints JSON without writing files.
param([string] $InspectTag = '')
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$outputRoot = Join-Path $projectRoot 'hil\production_20261012'
$validationPath = Join-Path $outputRoot 'evidence_validation.json'
$projectPrefix = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\') + '\'
$cases = @(
    @{tag='production_20261012_smoke_connected_1';seed=[uint64]1;seconds=30;capacity=8;negative=$false},
    @{tag='production_20261012_negative_control_1';seed=[uint64]1;seconds=6;capacity=8;negative=$true},
    @{tag='production_20261012_matrix_final_1';seed=[uint64]1;seconds=60;capacity=1;negative=$false},
    @{tag='production_20261012_matrix_final_2';seed=[uint64]305419896;seconds=60;capacity=8;negative=$false},
    @{tag='production_20261012_matrix_final_3';seed=[uint64]2779096485;seconds=60;capacity=32;negative=$false},
    @{tag='production_20261012_soak_final_1';seed=[uint64]3735928559;seconds=900;capacity=8;negative=$false}
)
$dailyTag = 'production_20261012_final_daily'
$expectedFixturePaths = @('hil\fw\Inc\production_stress.h','hil\fw\Src\production_stress.c',
    'hil\fw\Inc\production_irq.h','hil\fw\Src\production_irq.c','Core\Src\main.c',
    'hil\build_fw.ps1')

function Assert-Condition([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
}

function Get-ArtifactPath([string] $Name) {
    $path = [System.IO.Path]::GetFullPath((Join-Path $outputRoot $Name))
    Assert-Condition ($path.StartsWith($outputRoot + '\',
        [System.StringComparison]::OrdinalIgnoreCase)) "Unsafe artifact path: $Name"
    Assert-Condition (Test-Path -LiteralPath $path -PathType Leaf) "Missing artifact: $Name"
    return $path
}

function Read-ArtifactJson([string] $Name) {
    # Windows PowerShell 5.1 emits a top-level JSON array as one pipeline object.
    # Explicit iteration provides the same flat collection on 5.1 and PowerShell 7.
    $decoded = Get-Content -LiteralPath (Get-ArtifactPath $Name) -Raw | ConvertFrom-Json
    foreach ($item in $decoded) { $item }
}

function Get-ArtifactText([string] $Name) {
    Get-Content -LiteralPath (Get-ArtifactPath $Name) -Raw
}

function Get-ArtifactHash([string] $Name) {
    (Get-FileHash -LiteralPath (Get-ArtifactPath $Name) -Algorithm SHA256).Hash
}

function Get-Records([string] $Raw, [string] $Kind) {
    @([regex]::Matches($Raw, '(?m)^STRESS\|' + [regex]::Escape($Kind) + '\|[^\r\n]*') |
        ForEach-Object { $_.Value })
}

function Get-Fields([string] $Line) {
    $fields = @{}
    foreach ($match in [regex]::Matches($Line,
        '(?:^|[ |])(?<key>[a-z0-9_]+)=(?<value>[^ |\r\n]+)')) {
        $key = $match.Groups['key'].Value
        Assert-Condition (-not $fields.ContainsKey($key)) "Duplicate log field: $key"
        $fields[$key] = $match.Groups['value'].Value
    }
    return $fields
}

function Get-OneRecord([string] $Raw, [string] $Kind) {
    $records = @(Get-Records $Raw $Kind)
    Assert-Condition ($records.Count -eq 1) "Expected one $Kind record, found $($records.Count)"
    Get-Fields $records[0]
}

function Get-UInt([hashtable] $Fields, [string] $Key) {
    Assert-Condition ($Fields.ContainsKey($Key)) "Missing numeric field: $Key"
    Assert-Condition ($Fields[$Key] -match '^[0-9]+$') "Invalid unsigned field: $Key"
    $value = [uint64]0
    Assert-Condition ([uint64]::TryParse($Fields[$Key], [ref]$value)) "Overflow in field: $Key"
    return $value
}

function Get-SourceMap([object[]] $Entries, [int] $ExpectedCount, [string] $Label) {
    Assert-Condition ($Entries.Count -eq $ExpectedCount) "$Label source count mismatch"
    $map = @{}
    foreach ($entry in $Entries) {
        $relative = [string]$entry.path -replace '/', '\'
        $absolute = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
        Assert-Condition ($absolute.StartsWith($projectPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) "Unsafe source path: $relative"
        Assert-Condition (-not $map.ContainsKey($relative)) "Duplicate source: $relative"
        Assert-Condition ($entry.sha256 -match '^[A-Fa-f0-9]{64}$') "Invalid source hash: $relative"
        Assert-Condition (Test-Path -LiteralPath $absolute -PathType Leaf) "Missing source: $relative"
        $actual = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash
        Assert-Condition ($actual -eq $entry.sha256) "$Label source changed: $relative"
        $map[$relative] = $actual
    }
    return $map
}

function Assert-SameSources([hashtable] $Actual, [hashtable] $Expected, [string] $Label) {
    Assert-Condition ($Actual.Count -eq $Expected.Count) "$Label source map count mismatch"
    foreach ($path in $Expected.Keys) {
        Assert-Condition ($Actual.ContainsKey($path) -and ($Actual[$path] -eq $Expected[$path])) `
            "$Label source identity mismatch: $path"
    }
}

function Get-Provenance([string] $Tag, [int] $MinimumSeconds, [string] $Terminator) {
    $meta = Read-ArtifactJson "run_$Tag.json"
    $raw = Get-ArtifactText "log_$Tag.txt"
    $flash = Get-ArtifactText "flash_$Tag.txt"
    $savedHash = Get-ArtifactHash "firmware_$Tag.elf"
    Assert-Condition (($meta.tag -eq $Tag) -and $meta.flashed -and $meta.terminator_seen) `
        "Capture metadata is incomplete: $Tag"
    Assert-Condition (($meta.port -eq 'COM3') -and ($meta.baud -eq 115200)) `
        "Unexpected serial connection: $Tag"
    Assert-Condition (($meta.sha256 -match '^[A-Fa-f0-9]{64}$') -and
        ($savedHash -eq $meta.sha256)) "Saved ELF hash mismatch: $Tag"
    Assert-Condition ($meta.terminator -eq $Terminator) "Wrong capture terminator: $Tag"
    Assert-Condition (($flash -match 'File download complete') -and
        ($flash -match 'Download verified successfully') -and
        ($flash -match 'MCU Reset') -and
        ($flash -match '(?:Software|Hardware) reset is performed') -and
        ($flash -match 'ST-LINK SN\s*:\s*066DFF555775514867092907') -and
        ($flash -match 'Device name\s*:\s*STM32F446xx') -and
        ($flash -match 'Board\s*:\s*NUCLEO-F446RE')) "Missing actual board/Flash verification: $Tag"
    $elapsed = ([DateTimeOffset]::Parse($meta.finished) -
        [DateTimeOffset]::Parse($meta.started)).TotalSeconds
    Assert-Condition ($elapsed -ge $MinimumSeconds) "Capture wall time too short: $Tag"
    $actualFailures = @($raw -split '\r?\n' | Where-Object { $_ -match '\|FAIL(?:\||$)' })
    $metadataFailures = @($meta.failure_lines)
    Assert-Condition ($actualFailures.Count -eq $metadataFailures.Count) `
        "Failure metadata count mismatch: $Tag"
    for ($index = 0; $index -lt $actualFailures.Count; $index++) {
        Assert-Condition ($actualFailures[$index] -eq $metadataFailures[$index]) `
            "Failure metadata content mismatch: $Tag"
    }
    Assert-Condition ($raw -notmatch 'FAULT REPORT|NESTED FAULT|System halted\.') `
        "Unexpected hardware fault output: $Tag"
    return @{raw=$raw;meta=$meta;failures=$actualFailures;summary=[ordered]@{
        tag=$Tag;sha256=$savedHash;port=$meta.port;baud=$meta.baud;
        flash_verified=$true;elapsed_wall_seconds=$elapsed;
        log_sha256=(Get-ArtifactHash "log_$Tag.txt");
        flash_log_sha256=(Get-ArtifactHash "flash_$Tag.txt")
    }}
}

function Assert-CheckCounts([string] $Raw, [bool] $Negative) {
    $expected = @{
        payload_matches_host_golden=4;checksum_matches_host_golden=4;
        initialize_sixteen_workers_and_ipc=1;start_two_hardware_interrupt_sources=1;
        phase_irq_snapshot_start=3;phase_irq_snapshot_end=3;
        all_sixteen_workers_progress_each_phase=3;irq_backpressure_hit=1;
        task_send_timeout_hit=1;recovery_reaches_empty_queue=1;
        irq_stop_restores_peripherals=1;irq_stopped_snapshot=1;
        bounded_graceful_queue_drain=1;all_workers_acknowledge_shutdown=1;
        worker_stack_margin=16;worker_stack_magic_and_low_watermark=16;
        worker_is_suspended=16;worker_priority_restored=16;worker_role_has_real_result=16;
        controller_stack_margin=1;source_has_successful_send=6;
        every_successful_identity_validated_exactly_once=6;
        irq_semaphore_conservation=1;both_shared_semaphore_consumers_serviced=1;
        four_simultaneous_queue_receivers=1;blocking_queue_sender_exercised=1;
        two_simultaneous_semaphore_waiters=1;three_simultaneous_event_waiters=1;
        shared_resource_semaphore_contention=1;real_priority_inheritance_observed=1;
        event_any_all_timeout_exercised=1;synthetic_control_deadline=1;
        two_second_role_progress_watchdog=1;both_real_timer_irqs=1;
        actual_nvic_nested_preemption=1;irq_clean_stop=1;irq_context_and_register_contracts=1;
        all_fixture_wait_chains_empty=1;shared_mutex_released=1;all_resource_leases_returned=1;
        static_worker_delete=16;event_deinit=1;mutex_deinit=1;lease_sem_deinit=1;
        irq_sem_deinit=1;queue_deinit=1;static_fixture_preserves_heap=1;
        no_fixture_heap_allocation=1;all_runtime_contracts=1
    }
    if ($Negative) {
        $expected.every_successful_identity_validated_exactly_once = 5
        $expected.all_runtime_contracts = 0
    }
    $counts = @{}
    foreach ($line in @(Get-Records $Raw 'CHECK')) {
        $fields = Get-Fields $line
        Assert-Condition (($fields.status -eq 'PASS') -and $expected.ContainsKey($fields.name)) `
            "Unknown or failed CHECK line: $line"
        if (-not $counts.ContainsKey($fields.name)) { $counts[$fields.name] = 0 }
        $counts[$fields.name]++
    }
    foreach ($name in $expected.Keys) {
        $actual = 0
        if ($counts.ContainsKey($name)) { $actual = $counts[$name] }
        Assert-Condition ($actual -eq $expected[$name]) "CHECK multiplicity mismatch: $name"
    }
    $count = @(Get-Records $Raw 'CHECK').Count
    Assert-Condition ($count -eq $(if ($Negative) {159} else {161})) 'Wrong total CHECK count'
    return $count
}

function Get-StressValidation([hashtable] $Case) {
    $tag = $Case.tag
    $proof = Get-Provenance $tag $Case.seconds 'STRESS\|DONE\|'
    $raw = $proof.raw
    $manifest = Read-ArtifactJson "sources_$tag.json"
    $kernel = Get-SourceMap @($manifest.kernel) 61 $tag
    Assert-SameSources $kernel $baselineKernel $tag
    $fixtures = Get-SourceMap @($manifest.fixtures) 6 $tag
    foreach ($path in $expectedFixturePaths) {
        Assert-Condition ($fixtures.ContainsKey($path)) "Missing fixture identity: $path"
    }
    Assert-Condition (($manifest.configuration.seed -eq $Case.seed) -and
        ($manifest.configuration.seconds -eq $Case.seconds) -and
        ($manifest.configuration.capacity -eq $Case.capacity)) "Configuration identity mismatch: $tag"
    $begin = Get-OneRecord $raw 'BEGIN'
    $inject = $(if ($Case.negative) {1} else {0})
    Assert-Condition (((Get-UInt $begin seed) -eq $Case.seed) -and
        ((Get-UInt $begin seconds) -eq $Case.seconds) -and
        ((Get-UInt $begin queue_capacity) -eq $Case.capacity) -and
        ((Get-UInt $begin workers) -eq 16) -and ($begin.priorities -eq '5..20') -and
        ((Get-UInt $begin synthetic) -eq 1) -and ((Get-UInt $begin inject) -eq $inject)) `
        "BEGIN parameters differ from the required case: $tag"
    $build = Get-ArtifactText "build_$tag.txt"
    foreach ($definition in @('FIRMWARE_PRODUCTION_STRESS=1',
        "PRODUCTION_STRESS_SEED=$($Case.seed)","PRODUCTION_STRESS_SECONDS=$($Case.seconds)",
        "PRODUCTION_STRESS_QUEUE_CAPACITY=$($Case.capacity)","PRODUCTION_STRESS_INJECT_CORRUPTION=$inject")) {
        Assert-Condition ($build -match ('-D' + [regex]::Escape($definition) + '(?:U)?(?:\s|$)')) `
            "Required compile definition absent: $definition"
    }
    $disassembly = Get-ArtifactText "disassembly_$tag.txt"
    Assert-Condition (($disassembly -match '<production_irq_start>:') -and
        ($disassembly -match '<TIM6_DAC_IRQHandler>:') -and
        ($disassembly -match '<TIM7_IRQHandler>:')) "Timer fixture missing in recorded image: $tag"
    $phaseRecords = @(Get-Records $raw 'PHASE')
    Assert-Condition ($phaseRecords.Count -eq 3) "Missing three workload phases: $tag"
    $third = [uint64][Math]::Floor(($Case.seconds * 1000) / 3)
    for ($index = 0; $index -lt 3; $index++) {
        $phase = Get-Fields $phaseRecords[$index]
        $ticks = $(if ($index -eq 2) {($Case.seconds * 1000) - (2 * $third)} else {$third})
        Assert-Condition (((Get-UInt $phase phase) -eq ($index + 1)) -and
            ((Get-UInt $phase ticks) -eq $ticks)) "Phase duration/order mismatch: $tag"
    }
    $workers = @()
    foreach ($line in @(Get-Records $raw 'WORKER')) {
        $worker = Get-Fields $line
        $id = Get-UInt $worker id
        $completed = Get-UInt $worker completed
        Assert-Condition (($id -lt 16) -and ((Get-UInt $worker priority) -eq (5 + $id)) -and
            ((Get-UInt $worker heartbeat) -gt 0) -and ((Get-UInt $worker switches) -gt 0) -and
            ((Get-UInt $worker stack_free_words) -ge 32)) "Worker activity/stack contract failed: $line"
        Assert-Condition (($id -eq 13 -and $completed -eq 0) -or
            ($id -ne 13 -and $completed -gt 0)) "Worker role made no real progress: $id"
        $workers += $worker
    }
    Assert-Condition (($workers.Count -eq 16) -and
        (@($workers | ForEach-Object { $_.id } | Select-Object -Unique).Count -eq 16)) `
        "Missing or duplicate worker records: $tag"
    $controller = Get-OneRecord $raw 'CONTROLLER'
    Assert-Condition ((Get-UInt $controller stack_free_words) -ge 32) "Controller stack exhausted: $tag"
    $sources = @()
    $sentTotal = [uint64]0
    foreach ($line in @(Get-Records $raw 'SOURCE')) {
        $source = Get-Fields $line
        $id = Get-UInt $source source
        $sent = Get-UInt $source sent
        $validated = Get-UInt $source validated
        $prefix = Get-UInt $source prefix
        $pending = Get-UInt $source pending
        Assert-Condition (($id -lt 6) -and ($sent -gt 0)) "Invalid/idle data source: $line"
        if ($Case.negative -and ($id -eq 0)) {
            Assert-Condition (($sent -gt 3) -and ($validated + 1 -eq $sent) -and
                ($prefix -eq 3) -and ($pending -gt 0)) 'Negative source must show exactly one missing checksum identity'
        } else {
            Assert-Condition (($validated -eq $sent) -and ($prefix -eq $sent) -and ($pending -eq 0)) `
                "Message identity was lost/duplicated: $line"
        }
        $sentTotal += $sent
        $sources += $source
    }
    Assert-Condition (($sources.Count -eq 6) -and
        (@($sources | ForEach-Object { $_.source } | Select-Object -Unique).Count -eq 6)) `
        "Missing or duplicate source records: $tag"
    $receivedByWorkers = [uint64]0
    foreach ($worker in $workers) {
        $id = Get-UInt $worker id
        if (($id -ge 5) -and ($id -le 8)) {
            $receivedByWorkers += Get-UInt $worker completed
        }
    }
    # A rejected checksum packet was still received and completed by its consumer.
    Assert-Condition ($receivedByWorkers -eq $sentTotal) `
        "Consumer completion total differs from all successful sends: $tag"
    $sem = Get-OneRecord $raw 'SEMAPHORE'
    Assert-Condition (((Get-UInt $sem taken0) -gt 0) -and ((Get-UInt $sem taken1) -gt 0) -and
        (((Get-UInt $sem taken0) + (Get-UInt $sem taken1) + (Get-UInt $sem pending)) -eq
        (Get-UInt $sem accepted)) -and ((Get-UInt $sem pending) -le 64)) `
        "Semaphore conservation failed: $tag"
    Assert-Condition (((Get-UInt $sem taken0) -eq (Get-UInt ($workers | Where-Object id -eq 9) completed)) -and
        ((Get-UInt $sem taken1) -eq (Get-UInt ($workers | Where-Object id -eq 10) completed))) `
        "Semaphore service counters differ from worker results: $tag"
    $coverage = Get-OneRecord $raw 'COVERAGE'
    foreach ($minimum in @(@('queue_recv_waiters',4),@('queue_send_waiters',1),@('sem_waiters',2),
        @('lease_waiters',1),@('event_waiters',3),@('mutex_waiters',1),@('pi_observed',1),
        @('high_full',1),@('recovery_empty',1))) {
        Assert-Condition ((Get-UInt $coverage $minimum[0]) -ge $minimum[1]) `
            "Coverage minimum not reached: $($minimum[0]) ($tag)"
    }
    $control = Get-OneRecord $raw 'CONTROL'
    Assert-Condition (((Get-UInt $control period_ticks) -eq 10) -and
        ((Get-UInt $control deadline_ticks) -eq 10) -and ((Get-UInt $control jitter_max) -le 10) -and
        ((Get-UInt $control response_max) -le 10) -and ((Get-UInt $control misses) -eq 0) -and
        ((Get-UInt $control watchdog_misses) -eq 0)) "Control deadline/watchdog failed: $tag"
    $irq = Get-OneRecord $raw 'IRQ'
    Assert-Condition (((Get-UInt $irq entries0) -gt 0) -and ((Get-UInt $irq entries1) -gt 0) -and
        ((Get-UInt $irq nested) -gt 0) -and ((Get-UInt $irq depth_max) -eq 2) -and
        ((Get-UInt $irq forced) -gt 0) -and ((Get-UInt $irq errors) -eq 0)) `
        "Real interrupt/preemption coverage failed: $tag"
    $ipcAttempts = [uint64]0
    $entriesWithoutIpc = @()
    foreach ($index in 0,1) {
        $sent = Get-UInt $irq "sent$index"
        $rejected = Get-UInt $irq "rejected$index"
        $entries = Get-UInt $irq "entries$index"
        Assert-Condition ($entries -ge ($sent + $rejected)) `
            "IRQ IPC attempts exceed actual entries: $index ($tag)"
        $ipcAttempts += $sent + $rejected
        $entriesWithoutIpc += $entries - ($sent + $rejected)
        Assert-Condition ($sent -eq (Get-UInt ($sources | Where-Object source -eq (4 + $index)) sent)) `
            "IRQ/source sent counters differ: $index ($tag)"
    }
    Assert-Condition (((Get-UInt $sem accepted) + (Get-UInt $sem overflow)) -eq
        $ipcAttempts) "IRQ semaphore attempts differ from packet IPC attempts: $tag"
    $heap = Get-OneRecord $raw 'HEAP'
    Assert-Condition (((Get-UInt $heap before) -eq (Get-UInt $heap after)) -and
        ((Get-UInt $heap max_before) -eq (Get-UInt $heap max_after))) "Heap changed: $tag"
    $checkCount = Assert-CheckCounts $raw $Case.negative
    $summary = Get-OneRecord $raw 'SUMMARY'
    $done = Get-OneRecord $raw 'DONE'
    Assert-Condition ((Get-UInt $summary pass) -eq $checkCount) "Summary PASS count differs: $tag"
    $failRecords = @(Get-Records $raw 'FAIL')
    $injections = @(Get-Records $raw 'INJECT')
    if ($Case.negative) {
        Assert-Condition (($injections.Count -eq 1) -and
            ($injections[0] -eq 'STRESS|INJECT|checksum_corruption|source=0|sequence=3') -and
            ((Get-UInt $summary fail) -eq 3) -and ((Get-UInt $summary worker_errors) -eq 1) -and
            ($done.status -eq '-1')) 'Negative control must identify its injected checksum corruption'
        $failedChecks = @()
        $integrityReports = 0
        $firstReports = 0
        foreach ($failure in $failRecords) {
            if ($failure -eq 'STRESS|FAIL|packet_integrity checksum_errors=1') {
                $integrityReports++
            } elseif ($failure -eq 'STRESS|FAIL|first=PACKET_CHECKSUM actual=0 expected=6 errors=1') {
                $firstReports++
            } elseif ($failure -match '^STRESS\|FAIL\|check=(every_successful_identity_validated_exactly_once|all_runtime_contracts)$') {
                $failedChecks += $Matches[1]
            } else {
                throw "Unexpected failure cannot count as an expected negative result: $failure"
            }
        }
        Assert-Condition (($integrityReports -gt 0) -and ($firstReports -gt 0) -and
            ($failedChecks.Count -eq 2) -and (@($failedChecks | Select-Object -Unique).Count -eq 2)) `
            'Negative control is missing the expected specific diagnostics'
    } else {
        Assert-Condition (($proof.failures.Count -eq 0) -and ($failRecords.Count -eq 0) -and
            ($injections.Count -eq 0) -and ((Get-UInt $summary fail) -eq 0) -and
            ((Get-UInt $summary worker_errors) -eq 0) -and ($done.status -eq '0')) `
            "Normal capture failed: $tag"
    }
    $progress = @(Get-Records $raw 'PROGRESS')
    foreach ($line in $progress) {
        $fields = Get-Fields $line
        Assert-Condition ((Get-UInt $fields errors) -eq $(if ($Case.negative) {1} else {0})) `
            "Unexpected running errors: $line"
    }
    Assert-Condition ($progress.Count -ge 3) "Missing phase progress evidence: $tag"
    $proof.summary['classification'] = $(if ($Case.negative) {'expected_single_checksum_corruption'} else {'normal_pass'})
    $proof.summary['configuration'] = $manifest.configuration
    $proof.summary['checks_passed'] = $checkCount
    $proof.summary['checks_failed'] = Get-UInt $summary fail
    $proof.summary['worker_errors'] = Get-UInt $summary worker_errors
    $proof.summary['messages_successfully_sent'] = $sentTotal
    $proof.summary['messages_received_by_workers'] = $receivedByWorkers
    $proof.summary['workers'] = $workers
    $proof.summary['sources'] = $sources
    $proof.summary['semaphore'] = $sem
    $proof.summary['coverage'] = $coverage
    $proof.summary['control'] = $control
    $proof.summary['interrupts'] = $irq
    $proof.summary['inferred_entries_without_ipc'] = $entriesWithoutIpc
    $proof.summary['interrupt_counter_scope'] = 'Differences are computed from entry, sent and rejected counters. The log does not print spurious/natural/forced-request counters, so differences are not a directly observed cause classification.'
    $proof.summary['heap'] = $heap
    $proof.summary['manifest_sha256'] = Get-ArtifactHash "sources_$tag.json"
    return $proof.summary
}

function Get-DailyValidation {
    $proof = Get-Provenance $dailyTag 15 ''
    $daily = Read-ArtifactJson 'daily_validation.json'
    $dailyKernel = Get-SourceMap @(Read-ArtifactJson 'sources_final_daily.json') 61 'daily'
    Assert-SameSources $dailyKernel $baselineKernel 'daily'
    Assert-Condition (($daily.tag -eq $dailyTag) -and ($daily.sha256 -eq $proof.meta.sha256) -and
        ($daily.source_count -eq 61) -and $daily.matches_production_kernel_baseline -and
        ($daily.active_test_entries -eq 0) -and ($daily.active_test_rules -eq 0) -and
        ($daily.serial_capture_seconds -eq 15) -and $daily.flash_download_verified -and
        $daily.initialized -and ($daily.failure_lines -eq 0)) 'Daily restoration metadata failed'
    $bootIndex = $proof.raw.LastIndexOf('System init done', [System.StringComparison]::Ordinal)
    Assert-Condition ($bootIndex -ge 0) 'Daily capture has no initialization marker'
    # Keep the complete raw log/hash; accept periodic output only after the final boot.
    $bootText = $proof.raw.Substring($bootIndex)
    $task0Count = [regex]::Matches($bootText, '(?i)task[ _]0 running').Count
    $task1Count = [regex]::Matches($bootText, '(?i)task[ _]1 running').Count
    Assert-Condition (($proof.failures.Count -eq 0) -and
        ($task0Count -ge 2) -and ($task1Count -ge 2) -and
        ($bootText -notmatch '(?m)^(?:STRESS|BM|AU|ST|BT)\|')) 'Daily execution is not normal'
    Assert-Condition (($daily.task0_output_count -eq $task0Count) -and
        ($daily.task1_output_count -eq $task1Count) -and
        ($daily.acceptance_start -eq 'last System init done marker in the complete raw capture')) `
        'Daily task output metadata differs from the final boot segment'
    $symbols = Get-ArtifactText 'daily_symbols.txt'
    Assert-Condition ($symbols -notmatch ('(?m) (?:production_\w+|stress_\w+|bench_task|selftest_task|' +
        'boundary_test_task|test_scenario_task|perf_probes_run|lowlevel_\w+|flash_diag_\w+|' +
        'audit_\w+|__wrap_\w+)\r?$')) 'Daily ELF still contains a test entry'
    foreach ($rule in @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Debug') -Recurse -File |
        Where-Object { $_.Name -in 'subdir.mk','objects.list','makefile' })) {
        $ruleText = [System.IO.File]::ReadAllText($rule.FullName)
        Assert-Condition ($ruleText -notmatch ('FIRMWARE_(PERF|AUDIT|LOWLEVEL_CHECKS|' +
            'FLASH_DIAG|PRODUCTION_STRESS|BENCH|SELFTEST|BOUNDARY|SCENARIO)|PRODUCTION_STRESS_' +
            '|RTOS_HIL_AUDIT|--wrap=|hil/fwsrc|production_(stress|irq)')) `
            "Daily build retains a test rule: $($rule.FullName)"
    }
    Assert-Condition ((Get-FileHash -LiteralPath (Join-Path $projectRoot 'Debug\f446_rtos.elf')).Hash -eq
        $proof.meta.sha256) 'Current daily build differs from the restored board image'
    $proof.summary['classification'] = 'restored_daily_pass'
    $proof.summary['serial_capture_seconds'] = 15
    $proof.summary['source_count'] = 61
    $proof.summary['acceptance_start'] = $daily.acceptance_start
    $proof.summary['task0_output_count'] = $task0Count
    $proof.summary['task1_output_count'] = $task1Count
    $proof.summary['size_lines'] = $daily.size_lines
    $proof.summary['note'] = 'Inactive application USART/TIM6 hooks are allowed. USB register initialization is not proof of physical USB enumeration or transfer.'
    return $proof.summary
}

$baselineKernel = Get-SourceMap @(Read-ArtifactJson 'kernel_before.json') 61 'kernel baseline'
$currentKernelCount = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS') -Recurse -File |
    Where-Object { $_.Extension -in '.c','.h' }).Count
Assert-Condition ($currentKernelCount -eq 61) 'Unexpected current kernel source count'
if ($InspectTag -ne '') {
    $case = @($cases | Where-Object { $_.tag -eq $InspectTag })
    Assert-Condition ($case.Count -eq 1) 'InspectTag must identify a formal stress case'
    [ordered]@{scope='single_capture_inspection_only';overall_suite_validated=$false;
        capture=(Get-StressValidation $case[0])} | ConvertTo-Json -Depth 12
    exit 0
}

Assert-Condition (-not (Test-Path -LiteralPath $validationPath)) 'Do not overwrite final validation evidence'
# Require all formal metadata first; an incomplete suite must never create a PASS file.
foreach ($tag in @($cases | ForEach-Object { $_.tag }) + @($dailyTag)) {
    $null = Get-ArtifactPath "run_$tag.json"
}
$knownTags = @($cases | ForEach-Object { $_.tag }) + @($dailyTag)
$allRuns = @(Get-ChildItem -LiteralPath $outputRoot -Filter 'run_*.json' -File)
Assert-Condition ($allRuns.Count -eq 7) 'Unexpected/missing capture count requires manual review'
foreach ($run in $allRuns) {
    $runMeta = Get-Content -LiteralPath $run.FullName -Raw | ConvertFrom-Json
    Assert-Condition ($knownTags -contains $runMeta.tag) "Unreviewed extra capture: $($runMeta.tag)"
    Assert-Condition ($run.Name -eq "run_$($runMeta.tag).json") 'Capture filename/tag mismatch'
}
$validatedCaptures = @()
foreach ($case in $cases) { $validatedCaptures += Get-StressValidation $case }
$dailyResult = Get-DailyValidation
$attempt = Read-ArtifactJson 'attempt_smoke_initial.json'
Assert-Condition (($attempt.tag -eq 'production_20261012_smoke_initial_1') -and
    ($attempt.classification -eq 'not_executed_on_board') -and $attempt.build_succeeded -and
    (-not $attempt.flash_attempted) -and ($attempt.runtime_result -eq 'unavailable')) `
    'Initial attempt classification is unsupported'
foreach ($prefix in @('run','flash','log')) {
    $extension = $(if ($prefix -eq 'run') {'json'} else {'txt'})
    $initialPath = Join-Path $outputRoot "${prefix}_production_20261012_smoke_initial_1.$extension"
    Assert-Condition (-not (Test-Path -LiteralPath $initialPath)) `
        'Initial attempt has target execution evidence and cannot be excluded as unexecuted'
}
$strict = @(Read-ArtifactJson 'strict_compile\results.json')
Assert-Condition ($strict.Count -eq 8) 'Strict fixture compilation matrix is incomplete'
$strictIdentities = @{}
foreach ($unit in $strict) {
    $identity = "$($unit.name):$($unit.optimization)"
    Assert-Condition (-not $strictIdentities.ContainsKey($identity)) "Duplicate strict compile: $identity"
    $strictIdentities[$identity] = $true
    Assert-Condition (($unit.exit_code -eq 0) -and ($unit.warnings -eq 0) -and
        (@($unit.arguments) -contains '-Wall') -and (@($unit.arguments) -contains '-Wextra') -and
        (@($unit.arguments) -contains '-Werror')) "Strict fixture compile failed: $identity"
    if ($unit.name -in 'production_stress','production_irq') {
        $sourcePath = Join-Path $projectRoot "hil\fw\Src\$($unit.name).c"
        Assert-Condition ((Get-FileHash -LiteralPath $sourcePath).Hash -eq $unit.source_sha256) `
            "Strict compile source identity changed: $identity"
    }
}
foreach ($optimization in 'O0','O2','O3') {
    foreach ($name in 'production_stress','production_irq') {
        Assert-Condition ($strictIdentities.ContainsKey("${name}:$optimization")) 'Missing strict compile variant'
    }
}
foreach ($name in 'header_production_stress','header_production_irq') {
    Assert-Condition ($strictIdentities.ContainsKey("${name}:O2")) 'Missing self-contained header compile'
}
[ordered]@{
    validated_at = (Get-Date).ToString('o')
    status = 'passed_required_suite_with_expected_negative_control'
    required_stress_captures = 6
    normal_stress_passes = 5
    expected_negative_controls = 1
    restored_daily_captures = 1
    total_actual_board_captures = 7
    initial_unexecuted_attempts = 1
    normal_check_count_per_capture = 161
    kernel_source_count = 61
    kernel_matches_baseline_and_current_sources = $true
    strict_compile_units = 8
    strict_compile_warnings = 0
    normal_workload_seconds = 1110
    captures = $validatedCaptures
    daily = $dailyResult
    excluded_attempt = $attempt
    scope = 'Real STM32F446 board, shared IPC and timer IRQs; synthetic industrial inputs. This does not certify physical sensor/DMA/USB behavior or unrestricted WCET.'
} | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath $validationPath -Encoding utf8
Write-Host 'Verified: 5 normal stress passes, 1 specific expected negative control, 1 daily restoration; initial unexecuted attempt excluded.'
