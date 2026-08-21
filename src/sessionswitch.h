#ifndef SESSIONSWITCH_H
#define SESSIONSWITCH_H

// The one list of conversations: the tabs this window holds, the ones other
// windows are holding, and the ones on disk. Picking a row goes there —
// switching tabs, taking a session off another window, or resuming a past one
// as a new tab.
void sessionswitch_run(void);

// The window has just been asked to give a session up. Nonzero when it did and
// nothing is left, which is the caller's cue to leave.
int  sessionswitch_gave_last(void);
void sessionswitch_serve_request(void);

#endif
