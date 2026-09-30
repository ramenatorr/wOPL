#ifndef RETROBOX_LAUNCH_H
#define RETROBOX_LAUNCH_H

// retrobox: wOPL asks the retrobox brain what to launch (retro-gaming-box-poc
// docs/superpowers/specs/2026-09-30-ps2-opl-http-launch-design.md).

// IO thread, right after the SMB share connects: reads <prefix>retrobox.cfg and reports smb_connected.
void retroboxInit(const char *prefix);

// GUI thread, end of menuHandleInputMain (main list screen only): queues the ~2 s poll, acts on a received job.
void retroboxTick(void);

#endif
