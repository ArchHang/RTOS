set pagination off
set confirm off
set print pretty off
set trust-readonly-sections off
target extended-remote localhost:42123
monitor halt
printf "DGDBG|STATE|frozen=%u trigger=%u fault_valid=%u writing=%u session=%u first=%u next=%u\n", diag_frozen, diag_trigger, diag_fault_valid, diag_writing, diag_session, diag_session_first, diag_next_sequence
printf "DGDBG|FAULT|code=0x%x pc=0x%x cfsr=0x%x frame_valid=%u frame_pc=0x%x\n", diag_fault_code, diag_fault_pc, g_fault.cfsr, g_fault.frame_valid, g_fault.frame.pc
x/i g_fault.frame.pc
x/hx g_fault.frame.pc
set $slot_index = 0
while $slot_index < 64
    if diag_slots[$slot_index].commit >= diag_session_first
        printf "DGDBG|SLOT|index=%u commit=%u seq=%u tick=%u task=%u event=%u op=%u value=0x%x detail=0x%x result=%d\n", $slot_index, diag_slots[$slot_index].commit, diag_slots[$slot_index].record.sequence, diag_slots[$slot_index].record.tick, diag_slots[$slot_index].record.task_id, diag_slots[$slot_index].record.event, diag_slots[$slot_index].record.operation, diag_slots[$slot_index].record.value, diag_slots[$slot_index].record.detail, diag_slots[$slot_index].record.result
    end
    set $slot_index = $slot_index + 1
end
bt
detach
quit
