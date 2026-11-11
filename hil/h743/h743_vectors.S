/* Preserve EXC_RETURN and MSP. Cortex-M exception entries have no C frame. */
.syntax unified
.thumb

.section .text.SVC_Handler,"ax",%progbits
.global SVC_Handler
.type SVC_Handler, %function
.thumb_func
SVC_Handler:
    b.w rtos_port_svc_handler
.size SVC_Handler, .-SVC_Handler

.section .text.PendSV_Handler,"ax",%progbits
.global PendSV_Handler
.type PendSV_Handler, %function
.thumb_func
PendSV_Handler:
    b.w rtos_port_pend_sv_handler
.size PendSV_Handler, .-PendSV_Handler

.section .note.GNU-stack,"",%progbits
