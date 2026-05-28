#ifndef _COMP_DRV_H
#define _COMP_DRV_H

enum led_state {
    LED_OFF = 0,
    LED_ON = 1,
};

struct comp_drv_status {
    enum led_state state;
    unsigned long write_count;
};

#endif
