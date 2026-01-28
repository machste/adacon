#ifndef _TUI_H_
#define _TUI_H_

#include "adacom.h"

// Keys that are used in the TUI
#define TUI_KEY_ESC     27
#define TUI_KEY_DOWN  0402
#define TUI_KEY_UP    0403
#define TUI_KEY_LEFT  0404
#define TUI_KEY_RIGHT 0405
#define TUI_KEY_NPAGE 0522
#define TUI_KEY_PPAGE 0523


typedef void (*tui_action_cb)(int key);


void tui_init(void);
void tui_destroy(void);

void tui_add_action(int key, tui_action_cb cb);
void tui_add_num_action(tui_action_cb cb);

void tui_adacom_state(AdaComState state);
void tui_adacom_infos(const char *model, const char *sn, int num_channels);

int tui_select_channel(int channel);
void tui_set_attenuation(int channel, double value);
void tui_set_attenuations(double *values, int n);

#endif /* _TUI_H_ */
