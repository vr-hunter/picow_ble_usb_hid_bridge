#ifndef BRIDGE_H
#define BRIDGE_H

// One-time setup of the shared cross-core state (call from Core 0 before
// launching Core 1).
void bridge_init(void);

#endif // BRIDGE_H
