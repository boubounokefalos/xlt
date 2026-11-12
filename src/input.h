#ifndef INPUT_H
#define INPUT_H

#include <pspctrl.h>

typedef struct {
    unsigned int held;
    unsigned int pressed; /* new presses, plus auto-repeat for the d-pad */
} Input;

void input_init(void);
void input_poll(Input *in);

#endif
