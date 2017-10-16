#ifndef _LINUX_DOUBLETAP2WAKE_H
#define _LINUX_DOUBLETAP2WAKE_H

extern int dt2w_switch;
extern bool dt2w_scr_suspended;
extern bool in_phone_call;
extern unsigned int vib_strength;
extern unsigned int Dt2w_regions;

void doubletap2wake_setdev(struct input_dev *);

#endif	/* _LINUX_DOUBLETAP2WAKE_H */
