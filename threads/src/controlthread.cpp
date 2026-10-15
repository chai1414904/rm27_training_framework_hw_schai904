#include "threads.hpp"

#include "DJIMotorHandler.hpp"
#include "M2006.hpp"

extern CAN_HandleTypeDef hcan1;

namespace Threads
{
static TX_THREAD s_control_thread;
static UCHAR s_control_stack[kControlStackSize];

/* 电机对象和 handler 都放静态存储：注册关系要整个运行期保留 */
static M2006 s_m2006;
static DJIMotorHandler *const s_handler = DJIMotorHandler::Instance();

/* 调试开关：控制模式放在这里，便于按 V3 的要求【分别】验证三种模式。
 * 默认 RELAX_MODE —— 上电不出力，这是第一次上电唯一安全的默认值。
 * 用调试器改这个变量即可切换：0 = RELAX（松开）、1 = SPD（速度环）、2 = POS（双环）。 */
static volatile M2006::MotorControlModeType g_controlMode = M2006::RELAX_MODE;

void CreateControlThread(void)
{
    (void)tx_thread_create(&s_control_thread, (CHAR *)"control", ControlThreadEntry, 0,
                           s_control_stack, sizeof(s_control_stack),
                           kControlPriority, kControlPriority,
                           TX_NO_TIME_SLICE, TX_AUTO_START);
}

void ControlThreadEntry(ULONG input)
{
    (void)input;

    s_handler->registerMotor(&s_m2006, &hcan1, kMotorCanId);

    // 位置模式走的是"位置环套速度环"，也就是题目说的双环。
    // positionSet 给 0：位置模式下保持住开机时输出轴所在的位置。
    // 但模式由 g_controlMode 决定，默认先松开，确认反馈正常后再切换。
    s_m2006.controlMode = g_controlMode;
    s_m2006.positionSet = 0.0f;

    while (true)
    {
        // 1. 用本帧之前收到的反馈算出要发的电流
        s_m2006.setOutput();

        // 2. 上总线
        s_handler->sendControlData();

        // 3. 检查所有注册电机的在线状态
        s_handler->AllMotorAliveCheck();

        // 4. 掉线就松开：不能拿着过期的反馈继续做闭环
        if (s_m2006.MotorState != M2006::MOTOR_ONLINE)
        {
            s_m2006.controlMode = M2006::RELAX_MODE;
        }
        else
        {
            s_m2006.controlMode = g_controlMode;
        }

        // 心跳：告诉 alive 线程"我还活着"
        (void)tx_semaphore_put(&sem_control_beat);

        tx_thread_sleep(kControlPeriodTicks);
    }
}
} // namespace Threads
