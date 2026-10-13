#ifndef AUDIT_IPC_H_
#define AUDIT_IPC_H_

/* Run from the HIL audit controller task after the scheduler starts. */
void audit_ipc_run(void);

#endif
