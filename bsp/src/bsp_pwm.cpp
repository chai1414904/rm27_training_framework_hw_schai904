//
// Created by cosmosmount on 2025/9/2.
//

#include "bsp_pwm.hpp"

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim8;
extern TIM_HandleTypeDef htim10;

/**
 * @brief  取定时器的计数时钟频率（Hz）
 * @note   定时器挂在其所属 APB 上；若该 APB 预分频不为 1，定时器时钟 = PCLK × 2
 */
static uint32_t TimerCounterClock(TIM_HandleTypeDef *htim)
{
    TIM_TypeDef *inst = htim->Instance;

    if (inst == TIM1 || inst == TIM8 || inst == TIM9 || inst == TIM10 || inst == TIM11)
    {
        uint32_t ppre2 = (RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos;
        return (ppre2 == 0U) ? HAL_RCC_GetPCLK2Freq() : (HAL_RCC_GetPCLK2Freq() * 2U);
    }

    uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
    return (ppre1 == 0U) ? HAL_RCC_GetPCLK1Freq() : (HAL_RCC_GetPCLK1Freq() * 2U);
}

void PWM_Init(void)
{
    // "用户PWM" 排针引出的 7 路：TIM1_CH1~CH4 + TIM8_CH1~CH3
    PWM_Start(&htim1, TIM_CHANNEL_1);
    PWM_Start(&htim1, TIM_CHANNEL_2);
    PWM_Start(&htim1, TIM_CHANNEL_3);
    PWM_Start(&htim1, TIM_CHANNEL_4);
    PWM_Start(&htim8, TIM_CHANNEL_1);
    PWM_Start(&htim8, TIM_CHANNEL_2);
    PWM_Start(&htim8, TIM_CHANNEL_3);

    // BMI088 加热电阻：PF6 / TIM10_CH1
    PWM_Start(&htim10, TIM_CHANNEL_1);
}

void PWM_Start(TIM_HandleTypeDef *htim, uint32_t Channel)
{
    if (HAL_TIM_PWM_Start(htim, Channel) != HAL_OK)
    {
        Error_Handler();
    }
}

void PWM_Stop(TIM_HandleTypeDef *htim, uint32_t Channel)
{
    HAL_TIM_PWM_Stop(htim, Channel);
}

void PWM_SetPeriod(TIM_HandleTypeDef *htim, float period_s)
{
    // 计数频率 = 定时器时钟 / (预分频 + 1)；ARR = 计数频率 × 周期 − 1
    uint32_t counter_clk = TimerCounterClock(htim) / (htim->Init.Prescaler + 1U);
    uint32_t arr = (uint32_t)((float)counter_clk * period_s) - 1U;

    __HAL_TIM_SET_AUTORELOAD(htim, arr);
}

void PWM_SetDutyRatio(TIM_HandleTypeDef *htim, float dutyratio, uint32_t channel)
{
    // 把占空比约束在本函数约定的 [0, 1]，避免 CCR 越过 ARR 变成常高
    if (dutyratio < 0.0f)
    {
        dutyratio = 0.0f;
    }
    else if (dutyratio > 1.0f)
    {
        dutyratio = 1.0f;
    }

    uint32_t ccr = (uint32_t)(dutyratio * (float)(__HAL_TIM_GET_AUTORELOAD(htim) + 1U));

    __HAL_TIM_SET_COMPARE(htim, channel, ccr);
}
