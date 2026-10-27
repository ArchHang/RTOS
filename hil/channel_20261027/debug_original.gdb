# Reproduce the FIRST errors write in the archived ORIGINAL O0 image.
# This file is preparation only. The root operator starts ST-LINK_gdbserver,
# owns the board, then runs arm-none-eabi-gdb --nx -x <this-file> interactively.
# Do not use --batch: leave GDB connected and the target HALTED for inspection.
# No firmware is rebuilt. `load` flashes exactly the file selected below.
# Current C sources differ from this ELF: use DWARF values and /r disassembly,
# never current-source `list` output to identify the failing branch.

set pagination off
set confirm off
set print pretty on
set print elements 48
set print frame-info location-and-address
set remotetimeout 30
set tcp connect-timeout 30
set can-use-hw-watchpoints 1
set remote hardware-watchpoint-limit 1
set remote hardware-breakpoint-limit 2

file E:/STM32_Project/CubeIDE/f446_rtos/hil/channel_20261027/evidence/preflight/original_stress_failure/firmware_channel_stress_basepri_a1_O0.elf
set $original_errors_address = &'channel_stress.c'::errors
# This address was verified OFFLINE against this archived ELF's DWARF.
if $original_errors_address != 0x2000df48
    echo CH|GDB|ABORT|archived-errors-address-mismatch\n
    quit 2
end

set logging file E:/STM32_Project/CubeIDE/f446_rtos/hil/channel_20261027/evidence/preflight/original_stress_failure/gdb_watch_original.txt
set logging overwrite off
set logging redirect off
set logging enabled on
echo CH|GDB|SESSION|original-elf-no-rebuild\n
show version
info files
printf "CH|GDB|SYMBOL|errors_address=%p\n", $original_errors_address

define capture_original_state
    echo CH|GDB|CAPTURE|registers\n
    info all-registers
    echo CH|GDB|CAPTURE|backtrace-with-DWARF-locals\n
    bt full
    echo CH|GDB|CAPTURE|current-frame-arguments\n
    info args
    echo CH|GDB|CAPTURE|current-frame-locals\n
    info locals
    echo CH|GDB|CAPTURE|kernel-and-original-fixture-state\n
    p 'channel_stress.c'::errors
    p 'channel_stress.c'::checksum_errors
    p 'channel_stress.c'::produced
    p 'channel_stress.c'::consumed
    p 'channel_stress.c'::per_consumer
    p 'channel_stress.c'::channel.stats
    p rtos_kernel.tick_count
    p rtos_kernel.current_tcb
    p rtos_kernel.current_tcb->name
    p rtos_kernel.current_tcb->priority
    p rtos_kernel.current_tcb->state
    p rtos_kernel.current_tcb->blocked_on
    p rtos_kernel.current_tcb->wait_result
    p rtos_port_crit_nest
    p/x rtos_port_saved_mask
    # Named CPU mask registers depend on the server's XML register description.
    # Printing an unavailable convenience register yields void, not fabricated data.
    p/x $basepri
    p/x $primask
    p/x $control
    p/x $msp
    p/x $psp
    echo CH|GDB|CAPTURE|icsr-cfsr-hfsr-and-stack\n
    x/1wx 0xe000ed04
    x/3wx 0xe000ed28
    x/48wx $sp
    echo CH|GDB|CAPTURE|original-current-function-raw-instructions\n
    disassemble /r
    echo CH|GDB|CAPTURE|breakpoint-watchpoint-types\n
    info breakpoints
    echo CH|GDB|HALTED|no-continue-no-detach-no-quit\n
end

target remote 127.0.0.1:42123
# Loading is deliberately explicit, so flash bytes match the original DWARF.
load
monitor reset
compare-sections .isr_vector
compare-sections .text

# A temporary HARDWARE breakpoint avoids software breakpoint writes to flash.
# Stop at the exact audit entry before channel initialization or worker creation.
thbreak *audit_regressions_task
continue
echo CH|GDB|ARM|stopped-at-original-audit-entry\n
p/x $pc
p 'channel_stress.c'::errors

if 'channel_stress.c'::errors != 0
    echo CH|GDB|ABORT|errors-nonzero-before-audit-do-not-clear\n
    capture_original_state
else
    # File-qualified lookup avoids unrelated file-static variables named errors.
    # -location fixes the actual four-byte SRAM object, not a changing expression.
    watch -location *(unsigned int *)$original_errors_address
    commands
        silent
        echo CH|GDB|FIRST_ERROR_WRITE|hardware-watchpoint\n
        capture_original_state
        # Intentionally no continue: preserve the first increment's exact state.
    end

    # If this run does not reproduce errors++, stop instead of waiting forever
    # in ch_finish's park loop. This is NOT evidence that the old error is fixed.
    thbreak *ch_finish
    commands
        silent
        echo CH|GDB|NO_ERROR_WRITE_THIS_RUN|reached-original-ch_finish\n
        capture_original_state
    end
    info breakpoints
    echo CH|GDB|RUN|watching-first-original-errors-write\n
    continue
end

echo CH|GDB|READY|target-remains-halted-inspect-interactively\n
# To inspect the original caller manually: `up`, `info locals`, `disassemble /r`.
# To repeat, start a fresh GDB invocation of this file. Do not substitute Debug ELF.
# Keep the archived UART failure log even when a watched run does not reproduce.
