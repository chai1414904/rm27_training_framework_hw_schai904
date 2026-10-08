#include "bsp.hpp"

#include "bsp_can.hpp"
#include "bsp_pwm.hpp"
#include "bsp_usart.hpp"

void bsp_Init() {
    USART_Init();
    CAN_Init();
    PWM_Init();
}
