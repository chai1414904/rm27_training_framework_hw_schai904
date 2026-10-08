#include "BMI088.hpp"
#include "tx_api.h"
#include "bsp_pwm.hpp"
#include <cmath>

namespace BMI088
{
    /**
     * @brief BMI088 acc gyro 标定
     * @note 标定后的陀螺仪零偏存储在 Gyro_offset 中。
     * @attention 不管工作模式是blocking还是IT,标定时都是blocking模式,所以不用担心中断关闭后无法标定(RobotInit关闭了全局中断)
     * @attention 标定精度和等待时间有关。
     * @todo 将标定次数(等待时间)变为参数供设定
     * @section 整体流程为1.累加加速度数据计算gNrom()
     *                   2.累加陀螺仪数据计算零飘
     *                   3. 如果标定过程运动幅度过大,重新标定
     *                   4.保存标定参数
     */
    void cBMI088::Calibrate()
    {
        const int calib_samples = 4000; // 采样次数
        float gyro_sum[3] = {0.0f, 0.0f, 0.0f};
        gyro_data_t temp_gyro;

        // 1. 清除旧的 Offset，防止叠加
        Gyro_offset[0] = 0.0f;
        Gyro_offset[1] = 0.0f;
        Gyro_offset[2] = 0.0f;

        // 2. 循环采样
        for (int i = 0; i < calib_samples; i++)
        {
            ReadGyroData(&temp_gyro); // 这里读取的是原始值（因为Offset已清零）
            gyro_sum[0] += temp_gyro.x;
            gyro_sum[1] += temp_gyro.y;
            gyro_sum[2] += temp_gyro.z;
            
            tx_thread_sleep(1); // 等待一个 ThreadX tick；实际耗时由 tick 频率决定。
        }

        // 3. 计算平均值作为零偏
        Gyro_offset[0] = gyro_sum[0] / calib_samples;
        Gyro_offset[1] = gyro_sum[1] / calib_samples;
        Gyro_offset[2] = gyro_sum[2] / calib_samples;
        
        // 如果零偏过大（例如超过 0.1 rad/s），可能是在运动中标定的，应报错或丢弃
        if (fabs(Gyro_offset[0]) > 0.1f || fabs(Gyro_offset[1]) > 0.1f || fabs(Gyro_offset[2]) > 0.1f)
        {
            self_test.CALIBRATE_ERR = true;
            // 恢复默认值或保留上次值
            Gyro_offset[0] = BMI088_GYRO_PRE_CALI_OFFSET_X; 
            Gyro_offset[1] = BMI088_GYRO_PRE_CALI_OFFSET_Y;
            Gyro_offset[2] = BMI088_GYRO_PRE_CALI_OFFSET_Z;
        }
        else
        {
            self_test.CALIBRATE_ERR = false;
        }
    }

    void cBMI088::TemperatureControl(float target_temp)
    {
        // 本函数自己读温度：acc_data.temperature 没有被任何地方填充，不能依赖它
        float temp = 0.0f;
        ReadAccTemperature(&temp);

        TempPid.ref = target_temp;
        TempPid.fdb = temp;
        TempPid.UpdateResult();

        // TempPid 的 maxOut 是 25000，量纲不是占空比，所以必须在这里夹到 [0, 1]：
        // 6G 量程下 25000 相当于"几乎不限幅"，真正的限幅由这一句完成
        float duty = TempPid.result;
        if (duty < 0.0f)
        {
            duty = 0.0f;
        }
        else if (duty > 1.0f)
        {
            duty = 1.0f;
        }

        PWM_SetDutyRatio(&HEATING_RESISTANCE_TIM, duty, HEATING_RESISTANCE_CHANNEL);
    }

    void cBMI088::VerifyAccChipID()
    {
        uint8_t pRxData[2]; //< 读取两个字节，第一个是需要丢弃的无效字节，第二个是芯片 ID

        ReadReg(BMI088_CS_ACC, ACC_CHIP_ID_ADDR, pRxData, 2); //< 读取加速度计chip id
        tx_thread_sleep(1);
        //< 如果chip id不等于预设值,则加速度计ID错误,初始化错误
        if (pRxData[1] != ACC_CHIP_ID_VAL)
        {
            self_test.ACC_CHIP_ID_ERR = true;
            self_test.INIT_ERR = true;
        }
        else if (pRxData[1] == ACC_CHIP_ID_VAL)
        {
            self_test.ACC_CHIP_ID_ERR = false;
        }
    }

    void cBMI088::VerifyGyroChipID()
    {
        uint8_t pRxData;                                                 //< 读取一个字节,chip id
        ReadReg(BMI088_CS_GYRO, GYRO_CHIP_ID_ADDR, &pRxData, 1); //< 读取陀螺仪chip id
        tx_thread_sleep(1);
        //< 如果chip id不等于预设值,则陀螺仪ID错误,初始化错误
        if (pRxData != GYRO_CHIP_ID_VAL)
        {
            self_test.GYRO_CHIP_ID_ERR = true;
            self_test.INIT_ERR = true;
        }
        else if (pRxData == GYRO_CHIP_ID_VAL)
        {
            self_test.GYRO_CHIP_ID_ERR = false;
        }
    }

    void cBMI088::VerifyAccData()
    {
        bool bad = false;

        // ① NaN/Inf：一旦进入 PID 的积分项就再也清不掉（NaN + x 恒为 NaN）
        if (!std::isfinite(acc_data.x) || !std::isfinite(acc_data.y) || !std::isfinite(acc_data.z))
        {
            bad = true;
        }
        // ② 合加速度过小：真实器件永远至少测到重力，读数接近 0 说明 MISO 恒高/恒低，
        //    即 SPI 实际没有通。这一条同时能抓到"全 0x00"和"全 0xFF"两种卡死
        else if (std::sqrt(acc_data.x * acc_data.x + acc_data.y * acc_data.y + acc_data.z * acc_data.z) < 1.0f)
        {
            bad = true;
        }

        // 说明：这里没有做"单轴是否超出量程"的检查——raw 是 int16，
        // 换算后必然落在量程内，那种检查恒不成立，是无效代码。
        self_test.ACC_DATA_ERR = bad;
    }

    void cBMI088::VerifyGyroData()
    {
        bool bad = false;

        if (!std::isfinite(gyro_data.x) || !std::isfinite(gyro_data.y) || !std::isfinite(gyro_data.z))
        {
            bad = true;
        }
        else
        {
            // 陀螺仪静止时输出接近 0，不能像加速度计那样用"模长过小"判断。
            // 但结果不应超出量程太多：超出说明零偏异常（例如 Gyro_offset 未被合理初始化）
            // 或数据已损坏。留 1 rad/s 余量容纳零偏。
            const float full_scale = 32767.0f * IMU_GYRO_2000_SEN; // 2000°/s ≈ 34.9 rad/s

            if (std::fabs(gyro_data.x) > full_scale + 1.0f ||
                std::fabs(gyro_data.y) > full_scale + 1.0f ||
                std::fabs(gyro_data.z) > full_scale + 1.0f)
            {
                bad = true;
            }
        }

        self_test.GYRO_DATA_ERR = bad;
    }

    /**
     * @brief 操作加速度计或陀螺仪的片选
     * @param cs 片选编号
     * @param state GPIO_PIN_RESET 选中，GPIO_PIN_SET 释放
     */
    static void SelectCS(enum BMI088_SENSOR cs, GPIO_PinState state)
    {
        if (cs == BMI088_CS_ACC)
        {
            HAL_GPIO_WritePin(BMI088_CS_ACC_PORT, BMI088_CS_ACC_PIN, state);
        }
        else
        {
            HAL_GPIO_WritePin(BMI088_CS_GYRO_PORT, BMI088_CS_GYRO_PIN, state);
        }
    }

    void cBMI088::WriteReg(enum BMI088_SENSOR cs, uint8_t addr, uint8_t *data, uint8_t len)
    {
        // 写协议：地址最高位清零，紧跟数据；两步之间片选必须一直保持拉低
        uint8_t reg = addr & BMI088_SPI_WRITE_CODE;

        SelectCS(cs, GPIO_PIN_RESET);
        if (HAL_SPI_Transmit(&BMI088_SPI_HANDLE, &reg, 1, BMI088_SPI_TIMEOUT_MS) == HAL_OK)
        {
            if (data != nullptr && len > 0U)
            {
                (void)HAL_SPI_Transmit(&BMI088_SPI_HANDLE, data, len, BMI088_SPI_TIMEOUT_MS);
            }
        }
        SelectCS(cs, GPIO_PIN_SET);
    }

    void cBMI088::ReadReg(enum BMI088_SENSOR cs, uint8_t addr, uint8_t *data, uint8_t len)
    {
        if (data == nullptr || len == 0U)
        {
            return;
        }

        // 先把缓冲区清零：这样通信失败时调用方拿到的是确定的零值，
        // 而不是未初始化的数据（芯片 ID 校验依赖这一点）
        for (uint8_t i = 0U; i < len; ++i)
        {
            data[i] = 0U;
        }

        // 读协议：地址最高位置 1，随后原样读 len 字节。
        // 注意：加速度计在这 len 字节里，第一个是器件吐出的无效字节，
        //       由调用方丢弃（VerifyAccChipID 取 [1]、ReadAccData 显式跳过）；
        //       陀螺仪没有这个字节。本函数不做区分，只负责原始传输。
        uint8_t reg = addr | BMI088_SPI_READ_CODE;

        SelectCS(cs, GPIO_PIN_RESET);
        if (HAL_SPI_Transmit(&BMI088_SPI_HANDLE, &reg, 1, BMI088_SPI_TIMEOUT_MS) == HAL_OK)
        {
            (void)HAL_SPI_Receive(&BMI088_SPI_HANDLE, data, len, BMI088_SPI_TIMEOUT_MS);
        }
        SelectCS(cs, GPIO_PIN_SET);
    }

    void cBMI088::Config()
    {
        tx_thread_sleep(10); //< 等待系统稳定

        /*-------------------------------------加速度计初始化-------------------------------------*/
        
        //< 先软重启，清空所有寄存器
        uint8_t pTxData;
        pTxData = ACC_SOFTRESET_VAL;
        WriteReg(BMI088_CS_ACC, ACC_SOFTRESET_ADDR, &pTxData, 1);
        tx_thread_sleep(100); //< 延时100ms,重启需要时间

        //< 打开加速度计电源
        pTxData = ACC_PWR_CTRL_ON;
        WriteReg(BMI088_CS_ACC, ACC_PWR_CTRL_ADDR, &pTxData, 1);
        tx_thread_sleep(150);

        //< 加速度计变成正常模式
        pTxData = ACC_PWR_CONF_ACT;
        WriteReg(BMI088_CS_ACC, ACC_PWR_CONF_ADDR, &pTxData, 1);
        tx_thread_sleep(10); //

        //< 测量范围
        pTxData = ACC_RANGE_6G;
        WriteReg(BMI088_CS_ACC, ACC_RANGE_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = 0xAB;
        WriteReg(BMI088_CS_ACC, ACC_CONF_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = 0x08;
        WriteReg(BMI088_CS_ACC, INT1_IO_CTRL_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = 0x04;
        WriteReg(BMI088_CS_ACC, INT_MAP_DATA_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        /*-------------------------------------陀螺仪初始化-------------------------------------*/
        //< 先软重启，清空所有寄存器
        pTxData = GYRO_SOFTRESET_VAL;
        WriteReg(BMI088_CS_GYRO, GYRO_SOFTRESET_ADDR, &pTxData, 1);
        tx_thread_sleep(100); //< 延时100ms,重启需要时间

        pTxData = GYRO_RANGE_2000_DEG_S;
        WriteReg(BMI088_CS_GYRO, GYRO_RANGE_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = GYRO_ODR_2000Hz_BANDWIDTH_230Hz | GYRO_LPM1_SUS;
        WriteReg(BMI088_CS_GYRO, GYRO_BANDWIDTH_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = GYRO_LPM1_NOR;
        WriteReg(BMI088_CS_GYRO, GYRO_LPM1_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = GYRO_DRDY_ON;
        WriteReg(BMI088_CS_GYRO, GYRO_INT_CTRL_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = 0x00;
        WriteReg(BMI088_CS_GYRO, GYRO_INT3_INT4_IO_CONF_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms

        pTxData = 0x01;
        WriteReg(BMI088_CS_GYRO, GYRO_INT3_INT4_IO_MAP_ADDR, &pTxData, 1);
        tx_thread_sleep(5); //< 延时5ms
    }


    void cBMI088::ReadAccData(acc_data_t *data)
    {
        if (data == nullptr)
        {
            return;
        }
        *data = {};

        // 加速度计在地址之后会多吐一个无效字节，所以要多读一个：
        // raw[0] 丢弃，raw[1..2]=X，raw[3..4]=Y，raw[5..6]=Z，每轴低字节在前
        uint8_t raw[ACC_XYZ_LEN + 1];
        ReadReg(BMI088_CS_ACC, ACC_X_LSB_ADDR, raw, ACC_XYZ_LEN + 1);

        int16_t raw_x = (int16_t)(((uint16_t)raw[2] << 8) | (uint16_t)raw[1]);
        int16_t raw_y = (int16_t)(((uint16_t)raw[4] << 8) | (uint16_t)raw[3]);
        int16_t raw_z = (int16_t)(((uint16_t)raw[6] << 8) | (uint16_t)raw[5]);

        // IMU_ACCEL_6G_SEN 已经把 g 换算进去了，单位是 m/s²/LSB
        data->x = (float)raw_x * IMU_ACCEL_6G_SEN;
        data->y = (float)raw_y * IMU_ACCEL_6G_SEN;
        data->z = (float)raw_z * IMU_ACCEL_6G_SEN;
    }

    void cBMI088::ReadGyroData(gyro_data_t *data)
    {
        if (data == nullptr)
        {
            return;
        }
        *data = {};

        // 陀螺仪没有无效字节，raw[0..1]=X，raw[2..3]=Y，raw[4..5]=Z，每轴低字节在前
        uint8_t raw[GYRO_XYZ_LEN];
        ReadReg(BMI088_CS_GYRO, GYRO_RATE_X_LSB_ADDR, raw, GYRO_XYZ_LEN);

        int16_t raw_x = (int16_t)(((uint16_t)raw[1] << 8) | (uint16_t)raw[0]);
        int16_t raw_y = (int16_t)(((uint16_t)raw[3] << 8) | (uint16_t)raw[2]);
        int16_t raw_z = (int16_t)(((uint16_t)raw[5] << 8) | (uint16_t)raw[4]);

        // IMU_GYRO_2000_SEN 单位是 rad/s/LSB；Gyro_offset 由 Calibrate() 得到，同为 rad/s
        data->x = (float)raw_x * IMU_GYRO_2000_SEN - Gyro_offset[0];
        data->y = (float)raw_y * IMU_GYRO_2000_SEN - Gyro_offset[1];
        data->z = (float)raw_z * IMU_GYRO_2000_SEN - Gyro_offset[2];
    }

    void cBMI088::ReadAccTemperature(float *temp)
    {
        if (temp == nullptr)
        {
            return;
        }
        *temp = 0.0f;

        // 0x22 与 0x23 地址连续，一次读回来才能保证两个字节来自同一次采样：
        // 分两次读有可能读到跨采样点的拼接值。raw[0] 无效，raw[1]=MSB，raw[2]=LSB
        uint8_t raw[TEMP_LEN + 1];
        ReadReg(BMI088_CS_ACC, TEMP_MSB_ADDR, raw, TEMP_LEN + 1);

        // MSB 是温度高 8 位，LSB 的高 3 位补在低位，合成 11 位有符号量
        int16_t value = (int16_t)(((uint16_t)raw[1] << 3) | ((uint16_t)raw[2] >> 5));
        if (value > 1023)
        {
            value = (int16_t)(value - 2048); // 11 位补码 → 符号扩展到 16 位
        }

        *temp = (float)value * TEMP_UNIT + TEMP_BIAS;
    }

}

