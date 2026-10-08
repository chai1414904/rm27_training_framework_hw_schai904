#include "LED.hpp"
#include "main.h"

namespace LED
{
namespace
{
struct Pin
{
    GPIO_TypeDef *port;
    uint16_t pin;
};

/* 引脚取 CubeMX 生成的标签（board.ioc 里给 PH12/PH11/PH10 打了 GPIO_Label），
 * 这样模块里不出现硬编码的 GPIOH / GPIO_PIN_12 */
constexpr Pin kPins[3] = {
    {LED_R_GPIO_Port, LED_R_Pin}, // RED
    {LED_G_GPIO_Port, LED_G_Pin}, // GREEN
    {LED_B_GPIO_Port, LED_B_Pin}, // BLUE
};

/* C 板三色灯是高电平点亮（前序作业实测结论）。
 * 若某块板子实测相反，把这两个常量对调即可，其余代码不用动。 */
constexpr GPIO_PinState kLevelOn = GPIO_PIN_SET;
constexpr GPIO_PinState kLevelOff = GPIO_PIN_RESET;
} // namespace

void Init(void)
{
    for (const Pin &p : kPins)
    {
        HAL_GPIO_WritePin(p.port, p.pin, kLevelOff);
    }
}

void On(Color color)
{
    HAL_GPIO_WritePin(kPins[color].port, kPins[color].pin, kLevelOn);
}

void Off(Color color)
{
    HAL_GPIO_WritePin(kPins[color].port, kPins[color].pin, kLevelOff);
}

void Toggle(Color color)
{
    HAL_GPIO_TogglePin(kPins[color].port, kPins[color].pin);
}
} // namespace LED
