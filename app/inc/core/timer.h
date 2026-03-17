#ifndef INC_TIMER_H
#define INC_TIMER_H

// a function to initialize the timer or setup it

void timer_setup(void);

// Mention the pwm duty cycle once timer is setup.
void timer_set_pwm_duty_cycle(float duty_cycle);

#endif