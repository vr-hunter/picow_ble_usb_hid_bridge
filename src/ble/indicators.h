// Onboard LED and user button for the BLE host (Core 1). The LED reflects the
// pairing window and the number of bridged devices; a confirmed button press
// opens the pairing window.
#ifndef INDICATORS_H
#define INDICATORS_H

// Set up the button GPIO and start the LED and button timers (Core 1 init).
void indicators_init(void);

#endif // INDICATORS_H
