#include "timer.h"
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>

#define PRESCALER (84)
#define ARR_VALUE (1000)

void timer_setup(void){
    rcc_periph_clock_enable(RCC_TIM2); // RCC related to clock control, so when TIM2 is chosen, that should be controlled using the rcc register.
    //TIM2 to be used as the PWM timer, which takes in a reference clock signal.

    //Set the timer mode
    timer_set_mode(TIM2, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);

    //For pwm, as comparison with a value is required, we use oc mode
    timer_set_oc_mode(TIM2, TIM_OC1, TIM_OCM_PWM1);

    //Enable the counter 
    timer_enable_counter(TIM2);

    //Enable the oc output
    timer_enable_oc_output(TIM2, TIM_OC1);

    //next step to determine the PWM frequency and steps needed
    timer_set_prescaler(TIM2, PRESCALER-1);
    timer_set_period(TIM2, ARR_VALUE-1);
}

void timer_set_pwm_duty_cycle(float duty_cycle){
    const float raw_val = (float) (duty_cycle/100.0f) * ARR_VALUE;
    timer_set_oc_value(TIM2, TIM_OC1, (uint32_t)raw_val);
}