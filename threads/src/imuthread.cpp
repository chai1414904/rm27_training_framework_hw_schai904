#include "threads.hpp"

#include "BMI088.hpp"

namespace Threads
{
static TX_THREAD s_imu_thread;
static UCHAR s_imu_stack[kImuStackSize];

/* IMU 对象放静态存储：标定得到的陀螺零偏要一直保留，不能被重建 */
static BMI088::cBMI088 s_imu;

/* 最新一帧快照，供其它线程通过 GetImuSample() 读取 */
static ImuSample s_sample = {};

void CreateImuThread(void)
{
    (void)tx_thread_create(&s_imu_thread, (CHAR *)"imu", ImuThreadEntry, 0,
                           s_imu_stack, sizeof(s_imu_stack),
                           kImuPriority, kImuPriority,
                           TX_NO_TIME_SLICE, TX_AUTO_START);
}

void ImuThreadEntry(ULONG input)
{
    (void)input;

    // 1. 配置：写量程/带宽等寄存器，并校验两颗芯片的 ID（失败会置 INIT_ERR）
    s_imu.Config();

    // 2. 陀螺零偏标定。内部每次采样都会 sleep 一个 tick，所以耗时由
    //    tick 频率和标定次数共同决定（1 kHz 下 4000 次约 4 秒）
    s_imu.Calibrate();

    // 3. 报告初始化结果：芯片 ID 和标定都通过才算成功
    ReportImuInit(!s_imu.self_test.INIT_ERR && !s_imu.self_test.CALIBRATE_ERR);

    acc_data_t acc = {};
    gyro_data_t gyro = {};
    float temp = 0.0f;

    while (true)
    {
        // 加热到固定温度：陀螺零偏的主要来源是温漂，恒温后标定值才长期有效
        s_imu.TemperatureControl(kHeatTargetTemp);

        s_imu.ReadAccData(&acc);
        s_imu.ReadGyroData(&gyro);
        s_imu.ReadAccTemperature(&temp);

        // 校验函数读的是成员，所以先把本帧写回对象
        s_imu.acc_data = acc;
        s_imu.gyro_data = gyro;
        s_imu.VerifyAccData();
        s_imu.VerifyGyroData();

        // 发布快照
        ImuSample sample;
        sample.acc[0] = acc.x;
        sample.acc[1] = acc.y;
        sample.acc[2] = acc.z;
        sample.gyro[0] = gyro.x;
        sample.gyro[1] = gyro.y;
        sample.gyro[2] = gyro.z;
        sample.temperature = temp;
        sample.valid = !s_imu.self_test.ACC_DATA_ERR && !s_imu.self_test.GYRO_DATA_ERR;
        s_sample = sample;

        // 心跳：告诉 alive 线程"我还活着"
        (void)tx_semaphore_put(&sem_imu_beat);

        tx_thread_sleep(kImuPeriodTicks);
    }
}

void ReportImuInit(bool ok)
{
    g_imu_init_ok = ok;
    (void)tx_semaphore_put(&sem_init_done);
}

void GetImuSample(ImuSample *sample)
{
    if (sample == nullptr)
    {
        return;
    }
    *sample = s_sample;
}
} // namespace Threads
