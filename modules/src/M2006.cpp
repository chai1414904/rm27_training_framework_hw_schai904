#include "M2006.hpp"

M2006::M2006()
{
    // M2006 手册：允许电流对应反馈帧里的原始值上限 10000
    maxCurrent = 10000;
    gearBox = GearBox_M2006;

    // 速度环与位置环的增益和限幅放在基类的成员初始化器里（DJIMotor.hpp），
    // 那组值是 M2006 台架实测的；需要重新整定时用 speedPid.Tuning() / positionPid.Tuning()
    controlMode = RELAX_MODE;
}

void M2006::setOutput()
{
    switch (controlMode)
    {
    case RELAX_MODE:
        // 松开模式必须把 PID 的历史一起清掉，否则从松开切回闭环时，
        // 积分项还带着旧的累积值，会先冲一下再收敛
        speedPid.Clear();
        positionPid.Clear();
        currentSet = 0;
        break;

    case SPD_MODE:
        speedPid.ref = speedSet;
        speedPid.fdb = motorFeedback.speedFdb;
        speedPid.UpdateResult();
        currentSet = (int16_t)Numeric::LimitABS(speedPid.result, (float)maxCurrent);
        break;

    case POS_MODE:
        // 串级：位置环的输出当作速度环的目标（单位 rad/s），速度环的输出再作为电流
        positionPid.ref = positionSet;
        positionPid.fdb = motorFeedback.positionFdb;
        positionPid.UpdateResult();

        speedPid.ref = positionPid.result;
        speedPid.fdb = motorFeedback.speedFdb;
        speedPid.UpdateResult();
        currentSet = (int16_t)Numeric::LimitABS(speedPid.result, (float)maxCurrent);
        break;

    default:
        currentSet = 0;
        break;
    }
}

M2006::MotorStateTypedef M2006::AliveCheck()
{
    // 反馈每来一帧，UpdateSensorData 就会把 AliveFlag 加一。
    // 两次检查之间计数没变，说明这段时间没有收到任何反馈 → 判定离线。
    if (AliveFlag == Pre_AliveFlag)
    {
        MotorState = MOTOR_OFFLINE;
    }
    else
    {
        Pre_AliveFlag = AliveFlag;
        MotorState = MOTOR_ONLINE;
    }

    return MotorState;
}
