#ifndef RETROBOX_LAUNCH_H
#define RETROBOX_LAUNCH_H

// retrobox: wOPL asks the retrobox brain what to launch (retro-gaming-box-poc
// docs/superpowers/specs/2026-09-30-ps2-opl-http-launch-design.md).

// IO thread, right after the SMB share connects: reads <prefix>retrobox.cfg and reports smb_connected.
void retroboxInit(const char *prefix);

// GUI thread, end of menuHandleInputMain (main list screen only): queues the ~2 s poll, acts on a received job.
void retroboxTick(void);

// Any thread: one line in the step trace (<share>retrobox.log), only with "trace = 1" in retrobox.cfg.
void retroboxTrace(const char *fmt, ...);

// DEBUG BUILD (retrobox/debug-launch-trace): one line per step of a brain launch in <share>retrobox-launch.log, flushed
// line by line so the last line names where a hang happened. A no-op until rbActOnJob arms it.
void retroboxStep(const char *fmt, ...);
void retroboxStepEnd(void);
extern volatile int retroboxRenderTrace; // non-zero: trace the first frame drawn after the claim, element by element

#endif
