#include "DJIMotorHandler.hpp"
#include "bsp_can.hpp"

extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;

namespace
{
/// DJI 电机反馈帧 ID：0x201~0x208 对应 8 个槽位
constexpr uint16_t kFeedbackIdBase = 0x201U;
constexpr uint16_t kFeedbackIdMax = 0x208U;

/// 转子编码器为 13 位，一圈 8192 个计数
constexpr int16_t kEncoderTicks = 8192;
constexpr int16_t kEncoderHalfTicks = kEncoderTicks / 2;

constexpr float kTwoPi = 6.283185307179586f;

/**
 * @brief 减速箱速比（转子转数 : 输出轴转数）
 * @note  速比取自前序工程已验证的常数表：M2006 = 36、M3508 = 3591/187、XRoll = 268/17
 */
float GearRatio(GearBox gearBox)
{
    switch (gearBox)
    {
    case GearBox_M2006:
        return 36.0f;            // M2006 减速箱 36:1
    case GearBox_M3508:
        return 3591.0f / 187.0f; // M3508 减速箱 ≈ 19.2:1
    case GearBox_XRoll:
        return 268.0f / 17.0f;   // XRoll 减速箱 ≈ 15.76:1
    case GearBox_None:
    default:
        return 1.0f;             // 无减速箱（如 GM6020 直驱）
    }
}

/**
 * @brief 把 CAN 句柄映射成总线号
 * @return true 表示是本板使用的 CAN，bus 被写入
 */
bool BusOf(CAN_HandleTypeDef *hcan, uint8_t *bus)
{
    if (hcan == &hcan1) { *bus = 0U; return true; }
    if (hcan == &hcan2) { *bus = 1U; return true; }
    return false;
}
} // namespace

void DJIMotorHandler::registerMotor(DJIMotor *motor, CAN_HandleTypeDef *hcan, uint16_t canId)
{
    uint8_t bus;
    if (motor == nullptr || !BusOf(hcan, &bus))
    {
        return;
    }
    if (canId < kFeedbackIdBase || canId > kFeedbackIdMax)
    {
        return;
    }

    const uint8_t index = (uint8_t)(canId - kFeedbackIdBase);
    DJIMotorList[bus][index] = motor;

    // 槽位 0~3 由 0x200 控制帧驱动，4~7 由 0x1FF 驱动
    if (index < 4U)
    {
        if (bus == 0U) { CAN1_0x200_Exist = true; }
        else           { CAN2_0x200_Exist = true; }
    }
    else
    {
        if (bus == 0U) { CAN1_0x1FF_Exist = true; }
        else           { CAN2_0x1FF_Exist = true; }
    }
}

void DJIMotorHandler::sendControlData()
{
    uint8_t *const frames[2][2] = {
        {can1_send_data_0, can1_send_data_1},
        {can2_send_data_0, can2_send_data_1},
    };

    // 每帧装 4 个电机，每个电流 2 字节，大端序（高字节在前）
    for (uint8_t bus = 0U; bus < 2U; ++bus)
    {
        for (uint8_t half = 0U; half < 2U; ++half)
        {
            for (uint8_t slot = 0U; slot < 4U; ++slot)
            {
                DJIMotor *motor = DJIMotorList[bus][half * 4U + slot];
                const uint16_t current = (motor == nullptr) ? 0U : (uint16_t)motor->currentSet;

                frames[bus][half][slot * 2U] = (uint8_t)(current >> 8);
                frames[bus][half][slot * 2U + 1U] = (uint8_t)(current & 0xFFU);
            }
        }
    }

    // 只发含有已注册电机的那一帧；空帧不上总线
    CAN_HandleTypeDef *const bus_handle[2] = {&hcan1, &hcan2};
    const bool frame_exist[2][2] = {
        {CAN1_0x200_Exist, CAN1_0x1FF_Exist},
        {CAN2_0x200_Exist, CAN2_0x1FF_Exist},
    };
    const uint32_t frame_id[2] = {0x200U, 0x1FFU};

    for (uint8_t bus = 0U; bus < 2U; ++bus)
    {
        for (uint8_t half = 0U; half < 2U; ++half)
        {
            if (frame_exist[bus][half])
            {
                CAN_Transmit(bus_handle[bus], frame_id[half], frames[bus][half], 8U);
            }
        }
    }
}

void DJIMotorHandler::updateFeedback(CAN_HandleTypeDef *hcan, uint8_t *rx_data, int index)
{
    uint8_t bus;
    if (rx_data == nullptr || index < 0 || index > 7 || !BusOf(hcan, &bus))
    {
        return;
    }

    DJIMotor *motor = DJIMotorList[bus][index];
    if (motor == nullptr)
    {
        return; // 该槽位没有注册电机，丢弃这一帧
    }

    UpdateSensorData(motor, rx_data);
}

void DJIMotorHandler::UpdateSensorData(DJIMotor *motor, uint8_t *can_data)
{
    if (motor == nullptr || can_data == nullptr)
    {
        return;
    }

    DJIMotor::MotorFeedBack &fb = motor->motorFeedback;
    const float ratio = GearRatio(motor->gearBox);

    // DJI 反馈帧全部是大端（高字节在前）：
    // [0..1] 编码器、[2..3] 转子转速 rpm、[4..5] 实际电流、[6] 温度、[7] 保留
    fb.last_ecd = fb.ecd;
    fb.ecd = (uint16_t)(((uint16_t)can_data[0] << 8) | (uint16_t)can_data[1]);
    fb.speed_rpm = (int16_t)(((uint16_t)can_data[2] << 8) | (uint16_t)can_data[3]);
    fb.currentFdb = (float)(int16_t)(((uint16_t)can_data[4] << 8) | (uint16_t)can_data[5]);
    fb.temperatureFdb = (float)can_data[6];

    // 编码器转一圈就回绕，直接做差会在回绕点得到 ±8191 的错误跳变。
    // 先取有符号差值，超过半圈就认为实际是反向绕过来的，补一整圈。
    int16_t delta = (int16_t)(fb.ecd - fb.last_ecd);
    if (delta > kEncoderHalfTicks)
    {
        delta = (int16_t)(delta - kEncoderTicks);
    }
    else if (delta < -kEncoderHalfTicks)
    {
        delta = (int16_t)(delta + kEncoderTicks);
    }

    // 换算到输出轴：转子一圈 = 2π 弧度，除以减速比
    fb.lastPositionFdb = fb.positionFdb;
    fb.positionFdb += (float)delta * (kTwoPi / (float)kEncoderTicks) / ratio;

    // 转速反馈是转子 rpm，除减速比得到输出轴 rad/s
    fb.lastSpeedFdb = fb.speedFdb;
    fb.speedFdb = (float)fb.speed_rpm * (kTwoPi / 60.0f) / ratio;

    // 每收到一帧有效反馈就把在线计数加一，AliveCheck() 靠它判断通信是否还在
    ++motor->AliveFlag;
}

void DJIMotorHandler::AllMotorAliveCheck()
{
    for (uint8_t bus = 0U; bus < 2U; ++bus)
    {
        for (uint8_t slot = 0U; slot < 8U; ++slot)
        {
            if (DJIMotorList[bus][slot] != nullptr)
            {
                (void)DJIMotorList[bus][slot]->AliveCheck();
            }
        }
    }
}
