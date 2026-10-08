#include "threads.hpp"

#include "LED.hpp"

namespace Threads
{
/* 线程对象与栈都放静态存储，不占用 ThreadX 字节内存池 */
static TX_THREAD s_alive_thread;
static UCHAR s_alive_stack[kAliveStackSize];

TX_SEMAPHORE sem_init_done;
TX_SEMAPHORE sem_imu_beat;
TX_SEMAPHORE sem_control_beat;
volatile bool g_imu_init_ok = false;

/**
 * @brief 把信号量的计数全部取走，返回一共取到几次
 * @note  信号量是计数语义：被监控者每 put 一次计数加一，且 put 是累加的、不会丢。
 *        每次监控先排空，就得到"上次检查到现在发生过几次"，
 *        于是"这期间有没有发生"和"总共发生过几次"就被区分开了。
 *        这正是单靠一个计数器做不到、必须靠信号量计数来做的事。
 */
static UINT Drain(TX_SEMAPHORE *sem)
{
    UINT beats = 0U;
    while (tx_semaphore_get(sem, TX_NO_WAIT) == TX_SUCCESS)
    {
        ++beats;
    }
    return beats;
}

void CreateAliveThread(void)
{
    (void)tx_thread_create(&s_alive_thread, (CHAR *)"alive", AliveThreadEntry, 0,
                           s_alive_stack, sizeof(s_alive_stack),
                           kAlivePriority, kAlivePriority,
                           TX_NO_TIME_SLICE, TX_AUTO_START);
}

void AliveThreadEntry(ULONG input)
{
    (void)input;

    LED::Init();

    bool init_reported = false; // 是否已经拿到初始化结果（或已判超时）
    bool init_ok = false;
    ULONG waited_ticks = 0U;

    while (true)
    {
        const UINT init_beats = Drain(&sem_init_done);
        const UINT imu_beats = Drain(&sem_imu_beat);
        const UINT control_beats = Drain(&sem_control_beat);

        if (!init_reported)
        {
            if (init_beats > 0U)
            {
                init_reported = true;
                init_ok = g_imu_init_ok;
            }
            else
            {
                waited_ticks += kAlivePeriodTicks;
                if (waited_ticks >= kInitWaitTicks)
                {
                    // 等到上限还没等到，按初始化失败处理，之后进入常规监控
                    init_reported = true;
                    init_ok = false;
                }
            }
        }

        if (!init_reported)
        {
            // 初始化进行中（BMI088 标定本身要几秒）：红灯闪，另外两盏灭。
            // 这段时间不能因为收不到 imu 心跳就判它掉线——它正在长时间初始化。
            LED::Toggle(LED::RED);
            LED::Off(LED::GREEN);
            LED::Off(LED::BLUE);
        }
        else if (!init_ok)
        {
            // 初始化失败：红灯常亮，另外两盏灭
            LED::On(LED::RED);
            LED::Off(LED::GREEN);
            LED::Off(LED::BLUE);
        }
        else
        {
            // 初始化成功：红灯灭。
            // 绿灯跟 imu 心跳、蓝灯跟 control 心跳：有心跳就翻转，没心跳就熄灭。
            // 于是"哪一盏不闪/灭着"直接指出是谁掉了。
            LED::Off(LED::RED);

            if (imu_beats > 0U)
            {
                LED::Toggle(LED::GREEN);
            }
            else
            {
                LED::Off(LED::GREEN);
            }

            if (control_beats > 0U)
            {
                LED::Toggle(LED::BLUE);
            }
            else
            {
                LED::Off(LED::BLUE);
            }
        }

        tx_thread_sleep(kAlivePeriodTicks);
    }
}
} // namespace Threads

UINT Threads_CreateAll(void)
{
    UINT status;

    // initial_count 取 0：事件由 put 产生，不用预先置数
    status = tx_semaphore_create(&Threads::sem_init_done, (CHAR *)"init done", 0U);
    if (status != TX_SUCCESS)
    {
        return status;
    }

    status = tx_semaphore_create(&Threads::sem_imu_beat, (CHAR *)"imu beat", 0U);
    if (status != TX_SUCCESS)
    {
        return status;
    }

    status = tx_semaphore_create(&Threads::sem_control_beat, (CHAR *)"ctl beat", 0U);
    if (status != TX_SUCCESS)
    {
        return status;
    }

    Threads::CreateAliveThread();
    Threads::CreateImuThread();
    Threads::CreateControlThread();

    return TX_SUCCESS;
}
