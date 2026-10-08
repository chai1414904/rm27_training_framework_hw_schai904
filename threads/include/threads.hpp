#ifndef THREADS_HPP
#define THREADS_HPP

#include <cstdint>

#include "tx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建信号量并启动全部线程
 * @note  这是给 C 用的入口，由 app_threadx.c 的 App_ThreadX_Init 调用
 * @retval TX_SUCCESS 成功，否则是 ThreadX 的错误码
 */
UINT Threads_CreateAll(void);

#ifdef __cplusplus
}

/**
 * @brief 应用的三个线程与线程间通信
 *
 *   alive   : 监控初始化结果和另外两个线程的存活，用三色灯反映
 *   imu     : BMI088 初始化、标定，周期性读取加速度/角速度/温度
 *   control : 单电机串级 PID（位置环套速度环），反馈取自电机自身编码器
 *
 * 线程栈和线程控制块全部静态分配，不使用 ThreadX 字节内存池：
 * CubeMX 生成的 TX_APP_MEM_POOL_SIZE 只有 1024 字节，
 * 连一个线程的栈都放不下，更不能指望它装三个。
 */
namespace Threads
{
/**
 * @brief 把毫秒折算成 ThreadX tick
 * @note  写成函数而不是直接写 tick 数，是为了让周期定义与 tick 频率解耦
 */
constexpr ULONG MsToTicks(ULONG ms)
{
    return (ms * TX_TIMER_TICKS_PER_SECOND) / 1000U;
}

/* 三个线程的运行周期 */
constexpr ULONG kControlPeriodTicks = MsToTicks(1);   ///< 1 ms（1 kHz）
constexpr ULONG kImuPeriodTicks = MsToTicks(5);       ///< 5 ms（200 Hz）
constexpr ULONG kAlivePeriodTicks = MsToTicks(100);   ///< 100 ms（10 Hz）

/** 等 IMU 初始化完成的上限 */
constexpr ULONG kInitWaitTicks = MsToTicks(20000);    ///< 20 s

/* 这个前提必须成立，否则上面的 1 ms 会被折算成 0 个 tick，
 * 线程变成"不 sleep 地空转"，把 CPU 全占掉 */
static_assert(TX_TIMER_TICKS_PER_SECOND >= 1000U,
              "控制周期取 1 ms，要求 ThreadX tick 频率不低于 1 kHz（见 Core/Inc/tx_user.h）");

/* 优先级：数值越小越高。控制环最紧，监控最松 */
constexpr UINT kControlPriority = 8;
constexpr UINT kImuPriority = 12;
constexpr UINT kAlivePriority = 16;

/* 栈大小（字节） */
constexpr ULONG kControlStackSize = 1024;
constexpr ULONG kImuStackSize = 1536;
constexpr ULONG kAliveStackSize = 768;

/** 电机注册用的反馈帧 ID（M2006 编号 1） */
constexpr uint16_t kMotorCanId = 0x201U;

/** 加热目标温度，让陀螺零偏稳定 */
constexpr float kHeatTargetTemp = 40.0f;

/**
 * @struct ImuSample
 * @brief IMU 最新一帧的共享快照
 * @note  这是"尽力而为"的快照：imu 线程写、其它线程读，没有加锁。
 *        浮点写在 Cortex-M4 上是单条指令，不会读到半个数；
 *        但如果将来要用它参与闭环，应该加互斥或改成双缓冲。
 */
struct ImuSample
{
    float acc[3];      ///< m/s²
    float gyro[3];     ///< rad/s
    float temperature; ///< °C
    bool valid;        ///< 最近一次数据校验是否通过
};

void CreateAliveThread(void);
void CreateImuThread(void);
void CreateControlThread(void);

void AliveThreadEntry(ULONG input);
void ImuThreadEntry(ULONG input);
void ControlThreadEntry(ULONG input);

/**
 * @brief 取 IMU 最新一帧的副本
 */
void GetImuSample(ImuSample *sample);

/**
 * @brief 由 imu 线程在初始化结束时调用，报告初始化结果并发出初始化信号量
 */
void ReportImuInit(bool ok);

/* ---- 三个心跳信号量：被监控者 put，alive 线程排空后判断 ---- */
extern TX_SEMAPHORE sem_init_done;    ///< imu 线程初始化结束时 put 一次
extern TX_SEMAPHORE sem_imu_beat;     ///< imu 线程每轮 put 一次
extern TX_SEMAPHORE sem_control_beat; ///< control 线程每轮 put 一次

/** imu 线程写、alive 线程读的初始化结果 */
extern volatile bool g_imu_init_ok;
} // namespace Threads

#endif // __cplusplus

#endif // THREADS_HPP
