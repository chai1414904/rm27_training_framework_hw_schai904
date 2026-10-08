#ifndef LED_HPP
#define LED_HPP

/**
 * @brief C 板板载三色 LED 的简易封装
 * @note  只负责"点亮/熄灭/翻转"这三个动作。
 *        闪烁节奏、状态编码这类逻辑放在使用它的线程里（例如 alive 线程），
 *        因为那属于"怎么用灯"，不属于"怎么点灯"。
 */
namespace LED
{
/**
 * @enum Color
 * @brief C 板上三颗 LED 的颜色
 */
enum Color
{
    RED = 0,  ///< PH12
    GREEN = 1,///< PH11
    BLUE = 2, ///< PH10
};

/**
 * @brief 初始化：把三颗灯都熄灭，给出确定的起始状态
 */
void Init(void);

/**
 * @brief 点亮指定颜色的灯
 */
void On(Color color);

/**
 * @brief 熄灭指定颜色的灯
 */
void Off(Color color);

/**
 * @brief 翻转指定颜色的灯
 */
void Toggle(Color color);
} // namespace LED

#endif // LED_HPP
