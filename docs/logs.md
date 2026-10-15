# 调试与迭代记录
我写了一份log，AI写了一份log
作业在仓库中由AI编写后， 我又重新在AI的指导下写了一份
由于过程没有及时补充log.md,只能在事后写一份反思总结

这是AI给我的任务表
| 配置项 | 有对应代码吗 | 落在哪 | 验证手段 |
| --- | --- | --- | --- |
| **C1** 时钟 | ✗ 全生成 | —— | 公式推导 ＋ 实测 tick |
| **C2** GPIO / 灯 | ✓ | `modules/LED` | `run.ps1`：`led` 28 条 |
| **C3** TIM1 PWM | ✓ | `bsp_pwm` | 编译 ＋ 上板看舵机 |
| **C4** USART + DMA | ✓ | `bsp_usart` | 编译 ＋ 上板 |
| **C5** CAN1/CAN2 | ✓ | `bsp_can` | 编译 ＋ 上板看电机 |
| **C6** SPI1 | ✗ **没有 bsp 层** | 用法在 `modules/BMI088` | `bmi088` 56 条 ＋ **上板读 ID** |
| **C7** I2C3 | ✗ 更不用（IST8310 是选做）| —— | 上板 |
| **C8** TIM10 加热 | ✗ 没有新文件 | 用法在 `BMI088::TemperatureControl` | `bmi088` 里温度环那几条 |//
| **C9 / C10** ThreadX + 线程 | ✓ | `threads/` | ELF 符号 ＋ 上板三灯 |
| **C11** 堆栈 | ✗ | 链接脚本 | 与成品逐行对照 |
| **C12** `.c/.h pair` | ✗（已勾）| —— | —— |
| **C13** CMakeLists | ✗（自动）| —— | —— |



总结下来这次作业一共需要实现一下项目
Threads_CreateAll()                        ← 由 app_threadx.c 的 App_ThreadX_Init 调（C 调 C++ ✓ extern "C"）
    ├── DJIMotorHandler_Init()             → CAN_SetRxCallback(OnCanRx)      ★ 依赖注入
    ├── CreateAliveThread()                → AliveThreadEntry
    │       └── LED::Init/On/Off/Toggle     ＋ 三个信号量 Drain
    ├── CreateImuThread()                  → ImuThreadEntry
    │       └── cBMI088::Config/VerifyAccChipID/VerifyGyroChipID/Calibrate
    │           /ReadAccData/ReadGyroData/ReadAccTemperature/VerifyXxxData/TemperatureControl
    │               ├── ReadReg/WriteReg   → HAL_SPI_Transmit/Receive ＋ SelectCS(GPIO)
    │               └── PWM_SetDutyRatio   → HAL_TIM_SET_COMPARE      （bsp_pwm）
    └── CreateControlThread()              → ControlThreadEntry
            ├── M2006::setOutput()         → PID(speedPid/positionPid) ＋ Numeric::LimitABS
            ├── sendControlData()          → CAN_Transmit             （bsp_can）
            └── AllMotorAliveCheck()       → DJIMotor::AliveCheck()   （纯虚 ✓）
                                                ↑
CAN1_RX0_IRQHandler → HAL_CAN_IRQHandler → HAL_CAN_RxFifo0MsgPendingCallback
    └── s_rxCallback → OnCanRx → updateFeedback → UpdateSensorData
            └── 填 motorFeedback ＋ ++AliveFlag         这就是"在线检测"的信息来源

            ```
        ┌─────────────────────────────────────────────┐
        │  threads/   线程与并发                       │   ← 用：全部
        │  alive · imu · control                      │
        └───────────────┬─────────────────────────────┘
                        │
        ┌───────────────▼─────────────────────────────┐
        │  modules/   业务：设备语义                   │   ← 用：libs + bsp
        │  BMI088 · DJIMotor · DJIMotorHandler ·      │
        │  M2006 · LED · IMU(基类)                    │
        └────────┬───────────────────┬────────────────┘
                 │                   │
   ┌─────────────▼────────┐  ┌───────▼─────────────────┐
   │  libs/  纯算法        │  │  bsp/  板级外设封装      │
   │  pid · math · crc    │  │  pwm · usart · can       │
   └──────────────────────┘  └───────┬─────────────────┘
                                     │
                        ┌────────────▼───────────────┐
                        │  HAL + CubeMX 生成代码      │
                        │  Core/ · Drivers/ · CMSIS/ │
                        └────────────────────────────┘
`
```

**★ 三个"跨层"的关键点** ✓✓：
1. **`app_threadx.c`（C）调 `Threads_CreateAll`（C++）** ⇒ **必须 `extern "C"`** ✗（[T-119] ✓）
2. **CAN 接收是【中断上下文】** ⇒ **回调在 ISR 里跑** ✓
   ⇒ **① 不能调 `tx_*`** ✗ **② 花的是 MSP 那 1024 B** ✗（[T-149]/[T-155] ✓）
3. **`AliveFlag` 在 ISR 里加、在线判定在线程里读** ⇒ **这就是为什么它在检测"通信是否还在"** ✓

接下来列出个人与AI一起记录的要点：

### [T-06] 从 User Label 到宏，避免硬编码引脚

在 CubeMX 里给引脚起一个 User Label，生成的代码就会按“Label 加 `_Pin` 或者 `_GPIO_Port`”的规律给你生成宏。

```c
// main.h（CubeMX 生成）—— 规律：Label + _Pin / _GPIO_Port
#define LED_R_Pin        GPIO_PIN_12
#define LED_R_GPIO_Port  GPIOH
```

所以你的代码里应该写 `LED_R_Pin` 这种宏名，永远不要出现 `GPIO_PIN_12` 这样的裸引脚号。好处很直接：以后换引脚只需要在 CubeMX 里改一次，代码一行都不用动。


### [T-08] 模块化的判据：**改一个东西要动几处** —— 【做法】（分层顺序本身是【要求】）

模块化做得好不好，有一个可以检验的判据：改一个东西只波及一处，才算模块化做对了。如果改一个东西要动好几处，说明分层还没理清。

分层的依赖顺序是 `bsp`、`libs`、`modules`、`threads`，这个顺序本身就是作业原文点名的要求。每一层只知道自己那一摊事：

| 层 | 只管 | **不知道** |
| --- | --- | --- |
| `bsp/` | HAL 调用（CAN/UART/PWM 封装）| 电机、IMU 是什么 |
| `libs/` | **纯算法**（PID、数值、CRC）| 任何硬件 |
| `modules/` | **一类器件**（BMI088 / M2006 / LED）| 谁在用它、什么时候用 |
| `threads/` | **节奏与编排**（什么时候做什么）| 怎么点灯、怎么发 CAN |

| 要改什么 | 只该动 |
| --- | --- |
| 灯的**极性**（高亮/低亮）| **1 处**：`kLevelOn / kLevelOff` |
| 灯效的**含义**（谁闪代表什么）| **1 处**：`alivethread.cpp` |
| **引脚**变了 | **1 处**：CubeMX（宏自动更新）|

反过来说，如果在 `LED.cpp` 里把 `GPIOH / GPIO_PIN_12` 写死，换一个引脚就得翻遍代码去找，这就是没做到模块化的样子。

### [T-09] 怎么知道"哪里该改" —— 【做法】

要知道一处代码该不该改、能不能改，按文件来源分四类看就够了。

| 在哪 | 标记 | 规矩 |
| --- | --- | --- |
| **CubeMX 生成的代码** | **`USER CODE BEGIN … / END …`** | **只有块之间是你的**；**块外别改**（重新生成会覆盖）|
| **两个例外文件** | **没有标记** | `tx_user.h` / `tx_initialize_low_level.s` 整篇都可能被覆盖，所以改完必须 `git diff`（见 [T-04]）|
| **你自己写的文件**（`bsp/ libs/ modules/ threads/`）| **没有标记 —— 因为整篇都是你的** | "该改哪"靠 **[T-08] 分层**：要改什么，就只动对应那一层 |
| **你自己的注释** | 写"**为什么**"；必要时写"**换硬件改这里**" | 范例：`LED.cpp` 的 `kLevelOn` 注释 ——"若某块板子实测相反，把这两个常量对调即可" |

第一类是 CubeMX 生成的代码，认标记就行，只有块之间是你的。第二类是两个例外文件，它们没有标记，整篇都可能被覆盖，所以改完必须 `git diff` 确认，具体见 [T-04]。第三类是你自己写的文件，`bsp/`、`libs/`、`modules/`、`threads/` 下面通篇都是你的，判断该改哪里靠的是 [T-08] 的分层：要改什么就只动对应的那一层。第四类是你自己写的注释，注释里要写清楚为什么这么做，必要时写一句“换硬件改这里”；`LED.cpp` 里 `kLevelOn` 那条注释就是个好例子，它说明若某块板子实测极性相反，把这两个常量对调即可。

---

## C1 时钟树与 C2 三色灯（T-10 到 T-28）

## restart 练习工程 · C1 时钟树

### [T-10] 168 MHz 是怎么来的

板上那颗晶振 X1 是 12 MHz 的（见原理图），它先被 M 分频器除以 6 得到 2 MHz 的 VCO 输入。

这一步之所以定成 2 MHz，是因为 F407 要求 VCO 的输入落在 1 到 2 MHz 之间，2 正好卡在允许范围的上沿。

接着乘 N 等于 168，得到 336 MHz，再除以 P 等于 2，最终输出就是 168 MHz。

整个链条写下来就是下面的样子：

```c
HSE 12 MHz (原理图 X1)  → /M=6  → 2 MHz   (F407 要求 VCO 输入 1–2 MHz)
                        → ×N=168 → 336 MHz
                        → /P=2  → 168 MHz  ✓
```

### [T-11] 静默错误的活教材

前序工程里把 PLLM 设成了 8，算下来是 12 除以 8 再乘 168 再除以 2，结果是 126 MHz。

这个错误最坑的地方在于它一点都不响：编译不报错，串口照样能通，程序也照常跑，只是 CPU 慢了两成半。

因为外层看不出任何异常，所以这类配置错误往往要到很后面做性能相关的事情时才被发现，排查成本很高。

### [T-12] 新工程的两个默认值陷阱

新工程里 HSE 的默认值是 25 MHz，这个值在本板上是错的，必须手动改成 12。

还有一点容易踩坑：在时钟页面上根本选不了 HSE，这不是时钟页本身的问题，而是要先到 RCC 里把 HSE 设为 Crystal/Ceramic Resonator 把它使能了，时钟页才会给你这个选项。

## restart 练习工程 · C2 三色灯

### [T-20] 灯的 GPIO 三个取值及理由

| 设置 | 值 | 为什么 |
| --- | --- | --- |
| mode | 推挽 | 要能亮也要能灭，所以得能主动拉高拉低 |
| pull | 无上下拉 | 灯是主动驱动的，不需要默认电平 |
| speed | Low | 一秒才闪几次，高速只增功耗和干扰 |

### [T-21] 生成代码的顺序有意义

下面这两行的先后顺序不能颠倒：

```c
HAL_GPIO_WritePin(GPIOH, LED_R_Pin|LED_G_Pin|LED_B_Pin, GPIO_PIN_SET);  // ① 先设电平
GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;                             // ② 再配模式
```

先设好电平，再去配模式。反过来写的话，在"刚配上模式"的那一瞬间引脚电平是不确定的，会闪一下。

对灯来说这一下无所谓，但同样的顺序用在片选或者复位脚上就是事故了。

### [T-22] 复用冲突：PH10/11/12 同时是 TIM5_CH1/2/3

```
PH10 = TIM5_CH1   PH11 = TIM5_CH2   PH12 = TIM5_CH3
一个引脚同一时刻只能有一个身份：普通 GPIO 或 某个外设
```

这条规则的意思是，一个引脚同一时刻只能有一个身份，要么当普通 GPIO，要么属于某个外设，不能两边都占。

所以如果把 TIM5 开起来做 PWM，那三个灯就没法再用 GPIO 控制了。前序工程的 02_exti_tim 就因为这个原因丢过红色 LED。

正确的做法是换一个定时器，加热电阻用的就是 TIM10 的 CH1。

### [T-23] Label 里不能有空格

给引脚起 Label 的时候不能带空格。写成 `LED B` 的话，CubeMX 生成的宏就是 `LED B_Pin`，这不是一个合法的 C 标识符。

改用下划线就行，比如 `LED_B`。

### [T-24] 这一层是两层：点灯 vs 用灯

这一层其实分成了两件事，界线要划清楚：

| 层 | 管什么 | 落在哪 |
| --- | --- | --- |
| a 怎么点灯 | 亮、灭、翻转，落到引脚上就是一个动作 | `modules/LED.hpp` + `LED.cpp` |
| b 怎么用灯 | 什么状态显示什么灯效 | `threads/alivethread.cpp` |

这条界线写在你自己的注释里，`LED.hpp` 第 6 到 8 行就写着：这属于"怎么用灯"，不属于"怎么点灯"。

### [T-25] 不硬编码引脚（要求 D22）＋极性提成常量

引脚不写死，而是收进一张表，枚举值直接拿去当下标用，不需要 switch：

```cpp
constexpr Pin kPins[3] = {
    {LED_R_GPIO_Port, LED_R_Pin}, // RED  —— 枚举值直接当下标，不用 switch
    {LED_G_GPIO_Port, LED_G_Pin}, // GREEN
    {LED_B_GPIO_Port, LED_B_Pin}, // BLUE
};
/* C 板三色灯是高电平点亮（前序作业实测结论）；换极性相反的板子只改这两行 */
constexpr GPIO_PinState kLevelOn  = GPIO_PIN_SET;
constexpr GPIO_PinState kLevelOff = GPIO_PIN_RESET;
```

灯的极性也提成常量。C 板上的三色灯是高电平点亮，这是前序作业实测出来的结论；以后换一块极性相反的板子，只要改这两行就够了。

D22 的原话是：「自行实现 LED 模块（新文件，GPIO/SPI 连接按自己生成的板级配置填）」。

### [T-26] 驱动只暴露三个动作，没有 `Blink()`

驱动对外只给 Init、On、Off、Toggle 这几个动作，故意不提供 `Blink()`。

因为"闪"是时间层面的事情，一旦把它掺进驱动，驱动就和 RTOS 绑死了。

### [T-27] 灯效 = 一个三状态机

灯效用一个三状态机来描述，三种状态对应的显示如下：

| 状态 | 灯效 |
| --- | --- |
| 初始化进行中 | 红灯闪，其余灭 |
| 初始化失败 | 红灯常亮，其余灭 |
| 初始化成功 | 红灯灭；绿灯跟 imu 心跳翻转、蓝灯跟 control 心跳翻转；没心跳就熄灭 |

T1 的原话是：「`alive` 线程：用 `tx_semaphore_put` / `tx_semaphore_get` 监控三个对象——初始化是否成功、BMI088 线程是否存活、电机控制线程是否存活；通过灯效反映」。

### [T-28] 监控信号量的三条硬规矩

监控者取信号量时必须用 `tx_semaphore_get(..., TX_NO_WAIT)`，不能让出 CPU。因为一旦让出，这个"监控者"就变成了和被监控者同步，性质完全变了。

`Drain()` 的做法是把信号量全部取走并且计数，这样能区分"这期间有没有发生过"和"总共发生了几次"这两件不同的事。

还要有超时兜底：如果等 `sem_init_done` 超过 `kInitWaitTicks` 还没等到，就按失败处理，这样不会永远停在"初始化中"的状态上。

---

## SPI 与 I2C 基础 T-30 到 T-49

## SPI

### [T-30] CS 的两个身份

SPI 协议本身没有地址的概念，所以选设备只能靠拉低对方的 CS 引脚，这是硬件上的选择方式。同时 CS 不只起点名的作用，它还是这一次事务的边界：从机靠它判断这一整段通信属于同一次事务。这两件事在调试时都要放在心上。

### [T-31] 忘拉高 CS 的后果

如果一次读写结束时忘了把 CS 拉高，那么紧接着的下一次事务就会被拼成同一笔事务。这一点容易踩坑的地方在于，后果是双重的：一个从机会把不属于它的字节也听进去，而两个从机会同时驱动 MISO 线，直接打架。所以 CS 拉高这一步必须和拉低成对出现。

### [T-32] 四根线的方向

| 线 | 谁输出 | 从机那端 |
| --- | --- | --- |
| SCK / MOSI / CS | 主机 | 输入 |
| MISO | 从机 | 输出 |

因为 MISO 是唯一由从机输出的线，所以总线上的打架只可能发生在 MISO 上。也正因为这一点，没被选中的从机必须把自己的 MISO 放开，也就是进入高阻态。

### [T-33] 没有 ACK 靠回读

```cpp
// modules/src/BMI088.cpp —— SPI 版的"ACK"
ReadReg(BMI088_CS_ACC, ACC_CHIP_ID_ADDR, pRxData, 2);   // 读 2 字节：第 1 个是无效字节
if (pRxData[1] != ACC_CHIP_ID_VAL) { /* 置错误标志，交给 alive 线程灯效 */ }
```

SPI 没有 I2C 那样的应答机制，所以想要确认对方在不在、状态对不对，只能靠回读寄存器。上面这段就是一个例子：读两字节，第一个字节无效，用第二个字节和芯片 ID 的期望值比较。这种思路可以推广开——如果 SPI 想要额外的信息通道，就再加一根线，CS 和 INT 都是这么来的。

---

## I²C

### [T-40] 开漏的"高"是"放手"

$$
\text{输出低} = \text{下管合上，把线接到 GND} \qquad
\text{输出高} = \textbf{放手}（开漏没有上管）
$$

理解了开漏输出的物理动作，就能明白 I2C 上为什么谁都撞不到谁：任何一方输出高的时候只是放手，并不会主动把线推高，线的高低由上拉电阻恢复。真正会打架的是推挽输出，因为它有上管，会主动输出高电平，和别人的拉低顶在一起。

### [T-41] 选设备与 ACK

I2C 用地址来选设备。地址字节正好是 8 位，由 7 位地址加上 1 位读写方向位组成。至于 ACK，它是由收数据的那一方发出的，理解了这一点，才能看懂后面为什么每 8 位数据后面都跟着一位应答。

### [T-42] ACK 不是"结束传输"

ACK 并不表示传输结束。它只是每 8 位数据后面固定跟着的 1 位应答，用来告诉发送方这一字节收到了。真正结束一次传输靠的是停止条件。这两件事分开看，才不会把 ACK 误当成收尾信号。

### [T-43] 起始 / 停止

| | SCL | SDA |
| --- | --- | --- |
| 起始 | 高 | 高 → 低（被拉低）|
| 停止 | 高 | 低 → 高（放手，被上拉电阻恢复）|

这两种条件共同的前提是：SCL 为高的时候 SDA 必须保持稳定，不能变化。如果 SDA 确实需要变化，只能在 SCL 为低的时候变。正因为协议把"SCL 高时 SDA 跳变"专门留给了起始和停止，线上的数据位才不会被误判。

### [T-44] 一位是怎么传的

```
SCL 低 → 发送方改 SDA（摆好下一位）
SCL 高 → 接收方读 SDA（此时必须保持）
8 位之后 → 第 9 位是 ACK
数据位：最高位先发（MSB first）
```

流程可以这样记：SCL 为低时发送方把下一位摆到 SDA 上，SCL 抬高之后接收方来读，此时 SDA 必须保持不变。每传完 8 位，第 9 位是 ACK。顺便要记住数据位的顺序是最高位先发，也就是 MSB first。

### [T-45] I2C3 的配置与"开漏落地"

```c
// i2c.c
hi2c3.Init.ClockSpeed = 400000;              // 400 kHz
hi2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
hi2c3.Init.OwnAddress1 = 0;                  // 主机不需要地址 ⇒ 0
hi2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;   // 允许时钟拉伸
GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;      // ← 引脚：复用【开漏】
```

这里要留意最后一行，开漏这个结论最终是落在引脚模式上的，也就是 GPIO_MODE_AF_OD。作为对照，SPI 的引脚配的是复用推挽 AF_PP，因为 SPI 不需要线与，也就没必要用开漏。

---

## 验证方法与 C/C++ 补充：T-50 到 T-69

## restart 练习工程 · C3 PWM（控制舵机那一路）

### [T-60] 50 Hz 是怎么来的

配置里那两个减一，是为了把分频和计数的整数边界对上，算下来刚好落在 50 Hz 上：

```
TIM1.Prescaler = 168-1      168 MHz / 168   = 1 MHz
TIM1.Period    = 20000-1    1 MHz  / 20000  = 50 Hz    （周期 20 ms）
```

预分频器先按 168 分频，把 168 MHz 的时钟降成 1 MHz；然后计数周期取 20000，1 MHz 除以 20000 正好得到 50 Hz，也就是 20 ms 一个周期。

这个链条里真正好用的是计数单位：1 MHz 意味着一个计数就是一个微秒，所以比较寄存器 `CCR` 里的数值直接等于脉宽，单位就是微秒。想要 1.5 ms 的脉宽，把 `CCR` 写成 1500 就够了；想要 2.0 ms 就写 2000。

### [T-61] 舵机的控制量是脉宽，不是占空比

舵机的周期是固定 20 ms，不会因为你想要哪个角度而改变，角度只由高电平持续多久决定，常见的对应范围是 0.5 ms 到 2.5 ms。

所以思考舵机控制量的时候不要用占空比的百分比去想，舵机看的是绝对时间。硬要换算也能算出来，但是多绕一步，而且很容易在改周期的时候把两件事搅在一起，直接说脉宽更省事。

### [T-62] 舵机口在哪

这是第三个验收项指定的引脚，不能自己另选：

| 定时器 | 通道与引脚 |
| --- | --- |
| TIM1 | CH1 = PE9，CH2 = PE11，CH3 = PE13，CH4 = PE14 |
| TIM8（要求里写作"可加"） | CH1 = PC6，CH2 = PI6，CH3 = PI7 |

有一点容易踩坑的地方在于 `TIM10_CH1`：它是 PF6，但这个口是加热电阻用的，不要当成舵机口。

这和 LED 是同一个道理，一个引脚在同一时刻只能有一个身份，所以同一个引脚不能既是舵机口又是别的什么。

### [T-63] PWM 引脚是"先到先得"，成品里的引脚都是避开冲突后的选择

在 F407 上，TIM1 的每个通道都有多个候选引脚，比如 CH1 既可以是 PA8，也可以是 PE9，理论上挑哪个都行。

但是 PA8 已经被 I2C3_SCL 占用了，成品 `.ioc` 里写的是 `PA8.Signal=I2C3_SCL`，所以 CH1 只能选 PE9。CH2、CH3、CH4 是同一个道理，最后都走到 PE11、PE13、PE14 上。

TIM8 也一样，CH1 落在 PC6，而 CH2 和 CH3 选的是 PI6、PI7，这是为了避开 PC7、PC8 和 PC9，因为 PC9 是 I2C3_SDA。

由此可以归纳出三条。第一，一个引脚在同一时刻只能有一个身份。第二，配置顺序会影响可选的引脚，先配的外设先占位，后来的只能换脚。第三，成品里那些看起来奇怪的引脚，比如 PE9、PI6、PI7，都是这么来的，不是随手挑的。

### [T-64] "查手册"要分两种，原理图不等于数据手册

| 你想知道 | 查哪个 | 在哪 |
| --- | --- | --- |
| 板子上这根线接到哪里（排针、器件），有没有上拉电阻、阻值多大，LED 是高电平点亮还是低电平，CAN 终端电阻 | 原理图（`resources/.../C 型原理图.pdf`） | 已提取成 `原理图提取/data/schematic_text.txt`（全文可搜），并蒸馏成 `作业追踪/C板引脚映射-已核实.md` |
| MCU 的某个脚能当什么功能（AF 表） | 数据手册 Table 9，或者 CubeMX（更快） | 工作区里没有 MCU 数据手册，要去 ST 官网下载 |

这两件事分清楚的必要在于：原理图里没有 AF 表，所以"PE9 能当 TIM1_CH1"这件事原理图证明不了，只能查数据手册。

另外原理图 PDF 没有文本层，不能按下 Ctrl+F 直接搜，所以才要靠上面那两个提取产物。

### [T-65] 写完函数还要问"谁调用它"

这是本仓库里代价最贵的一个坑，也是仓库规范 §4 专门记下的一条：`bsp_Init()` 定义了，但从头到尾没有任何人调用它，于是它内部的 `HAL_CAN_Start` 和 `HAL_TIM_PWM_Start` 被 `--gc-sections` 整个丢掉，结果是电机全死、加热不出力。

最麻烦的是当时的检查全部通过：173 项主机断言过了，评审模拟构建也过了，一个都没抓到。原因在于单元测试是直接调用被测函数的，它天然假设调用方存在，所以它测得出函数体里算得对不对，测不出有没有路径能走到这个函数。

手段是用 `arm-none-eabi-nm board.elf` 去找函数名，如果它不在 ELF 里，就说明没有任何路径能到它，这比看 FLASH 有没有涨要可靠。

推广开来就是一句话："能编译"和"会被执行"是两件事。

### [T-66] "没有 `tim.c`"，是 C12（`.c/.h pair`）没勾

现象是 CubeMX 里明明配了 TIM1，生成的 `main.c` 里也有 `MX_TIM1_Init()`，但就是找不到 `tim.c`。

原因是这个开关决定外设初始化代码是"每个外设一个 `.c/.h`"还是"全部塞进 `main.c`"。在 `.ioc` 里看 `ProjectManager.CoupleFile=true` 就是分开生成，会得到 `tim.c`、`gpio.c`、`spi.c`、`i2c.c`、`usart.c`、`can.c`；如果没有这一行，默认就是 false，所有初始化都写在 `main.c` 里，所以自然找不到 `tim.c`。

开关的位置在 Project Manager，进去选 Code Generator，勾上 "Generate peripheral initialization as a pair of '.c/.h' files per peripheral"。

要留意的是勾上它之后会一次性重排很多文件，所以勾之前先 `git commit`，这样重排完能看清到底动了什么。

教训是讲"去哪个文件找"之前，先确认那个文件会存在。我自己犯过这个错：讲 `main.h` 的时候还提过"C12 没勾就在 `main.c`"，讲 C3 的时候却直接说"去 `tim.c`"。

## 验证 / 检查

### [T-50] 主机测试测不出什么

173 项断言覆盖的是"算得对不对"这一类问题，比如一段换算、一个状态机的分支走法。

时钟频率配错了、外设到底有没有真的连通、CAN 报文的 ID 和字节序对不对，这些它测不出来，只能上板看。

### [T-51] 没失败过的检查器 = 没验证过

一个检查脚本如果从来没有报过错，你就不知道它是真的在检查，还是它根本没生效。

所以检查脚本必须做故障注入来证明它会响。本工作区的实例是给 `progress.ps1` 造了一棵假树，四类故障全部被报出来，退出码也是 1，这就说明它确实在工作。

### [T-52] 复查要对着工具，不要对着"自己的清单"

这条来自 2026-10 的两次实际教训，两次都发生在我身上。

第一次是复查用户代码时，我只对着自己列的那 8 项逐条看，结果漏掉了"头文件保护没闭合"，也就是缺 `#endif`。这个是用户的 VS Code Problems 面板抓出来的，报的是 `unterminated #ifndef` 和 `missing #endif`。第二次是同一轮还漏了"一行只改了一半"：`kPins[color].port` 改了，但 `kPin[color].pin` 没改。

所以写完或者复查的第一个动作应该是跑工具，包括编译器、Problems 面板、`测试/*.ps1`，而不是"再看一遍"。我的清单只能证明清单内的事，清单外的东西只能靠工具兜。

还有一点要单独记下来：工具的盲区要单独记，比如 `src` 和 `Src` 这类大小写问题，Windows 上的编译器和 Problems 面板都抓不到，只在 Linux 或者 CI 上才会暴露出来，本项目已经踩过一次同类坑。

## 补充（C / C++ 与链接）

### [T-67] `multiple definition` 的机制

一句话说，就是同一个符号被链接器在两个以上的 `.obj` 里看到了。

这一点容易踩坑的地方在于：头文件保护，也就是 `#ifndef` 和 `#pragma once`，挡不住它。头文件保护只在单个编译单元内部起作用，而重复定义是链接期的事，那时候编译单元早就各自编完了。

常见的触发方式有五种。第一种是函数体写在头文件里又没加 `inline`，于是每个 include 它的 `.cpp` 都各定义一份。第二种是变量定义写在头文件里，比如 `int cnt = 0;`，要改成 `extern` 声明加一个 `.cpp` 定义。第三种是同一个 `.cpp` 被加进构建两次，比如 `target_sources` 写了两遍，或者 `GLOB` 与手写重复。第四种是两个 `.cpp` 定义了同名函数，通常是复制粘贴忘了删。第五种最隐蔽：某个 `.cpp` 里 `#include` 了另一个 `.cpp`。

第五种的症状认得最快：报错里"重复的那份"和"第一次定义的那份"指向同一个源文件，一处写成 `.obj`，一处写成源码行号，因为编译器会把被包含文件的行号一起带进来。2026-10 的实例是 `bsp.cpp` 里写了 `#include "bsp_pwm.cpp"`，于是 `bsp_pwm.cpp.obj` 和 `bsp.cpp.obj` 各定义一份，5 个函数全部报 `multiple definition`。修法是把 `#include "xxx.cpp"` 改成 `#include "xxx.hpp"`，也就是声明与定义分开。

定位的时候 GNU LD 会同时列出两处，看 `first defined here` 那一行。Problems 面板只显示一条，路径还会被截断，所以完整输出要去 Terminal 或 Output 里看。

还有一个反直觉的点：不要用"链接命令行里 `.obj` 只出现一次"来排除重复定义。`#include "*.cpp"` 造成的重复藏在两个 `.obj` 内部，链接行本身是干净的。我在这一条上推错过一次，据此判定"报错是陈旧的"，方向完全反了。

### [T-68] C / C++ 混编必须 `extern "C"`

症状是 `main.c`（C）调用 `bsp.cpp`（C++）里的函数时，先出现 `implicit declaration` 警告，接着报链接错误 `undefined reference`。

原因是 C++ 会给函数改名，也就是 name mangling，而 C 那边是按原名去找的，所以找不到。

解法要落到三处，缺一处都不行：

```cpp
/* bsp.hpp —— 声明处 */
#ifdef __cplusplus
extern  "C" {
#endif
    void bsp_Init(void);
#ifdef __cplusplus
}
#endif
```

第一，`bsp.hpp` 里用 `extern "C"` 把声明包起来，成品就是这么写的。第二，`.cpp` 里要 `#include "bsp.hpp"`，这一条不能省，因为只有定义看见了 C 链接的声明，它才会按 C 的方式导出符号，只在调用方声明是不够的。第三，`main.c` 也要 include 这个头文件，放在 `USER CODE BEGIN Includes` 里。

### [T-69] 工具自己也会骗你：`nm` 查 C++ 符号要 `--demangle`

这是 2026-10 的实例，我连错两次。

第一次是用 `nm board.elf | Select-String "PWM_Init"` 判断符号在不在，结果搜不到，因为 C++ 的函数名是 mangle 过的，实际是 `_Z8PWM_Initv`，按朴素的名字搜当然搜不到。我据此得出了"符号不在 ELF 里"的错误结论，而实际上它在。第二次是重写检查时括号不配对，命令直接报语法错误，报的是 `MissingEndParenthesisInExpression`，又白跑一次。

正确的写法是让 `nm` 先把名字还原成可读形式再搜：

```powershell
arm-none-eabi-nm --demangle board.elf | Select-String "PWM_Init"
```

推广开来，"工具说没有"和"我搜法不对"要分清楚。报"找不到"的时候，先问一句：是不是我的查询方式有问题。另外还要知道，"不在 ELF 里"这件事本身也可能是假象，虚函数就是一类例子。

---

## C4 串口 T-70 到 T-95

## restart 练习工程 · C4 UART（把两个串口都打开）

### [T-70] 板上丝印“UART1”不等于 `USART1`

板子丝印上写的名字会骗人，所以引脚和串口编号一律以 `.ioc` 配置和原理图为准，不要看着丝印就下结论。
你工作区的 [`C板引脚映射-已核实.md`](../作业追踪/C板引脚映射-已核实.md) §3.3 已经记过这一条。

### [T-71] USART1 的 RX 为什么是 `PB7`、不是 `PA10`

这又是 [T-63] 讲的“先到先得”。F407 上 USART1_RX 有 `PA10` 和 `PB7` 两个候选引脚，成品选了 `PB7`。
原因是 `PA10` 在那之前已经被别的外设占走了。具体是谁占的，去 CubeMX 里点一下 `PA10` 看它的菜单就行，这一点不要靠猜。

### [T-72] 串口为什么要配 DMA

这是作业原文点名要求的，bsp 那条检查项写的是“检查句柄、数据长度、中断与 DMA 配置”。
如果不用 DMA，每个字节都要 CPU 进一次中断去搬，几 kHz 的打印量就足以拖住控制环；用了 DMA 之后，硬件自己把数据搬进内存，CPU 只在搬完的时候被通知一次。
这里容易踩坑的地方在于 `Mode` 要选 `NORMAL` 而不是 `CIRCULAR`，也就是收一次停一次。这对定长帧正合适，如果你想让它“永远在收”，得自己重新装填一次。
另外 DMA 的 Stream 号同样是先到先得的。成品里的分配是：USART1_RX 用 DMA2_Stream2，USART1_TX 用 Stream7，USART6_RX 用 Stream1，USART6_TX 用 Stream6。

### [T-73] 一个发送函数带 `mode`，而不是三个函数

```cpp
enum USART_Mode { USART_MODE_BLOCK = 0, USART_MODE_DMA = 1, USART_MODE_IT = 2 };
void USART_Transmit(UART_HandleTypeDef *huart, uint8_t *pData, uint16_t Size, enum USART_Mode mode);
```

函数内部用一个 `switch(mode)` 分别去调 `HAL_UART_Transmit`、`_DMA` 或 `_IT`。
这样做的好处是调用点只写一个模式就够了，不用记三个函数名。

### [T-74] 阻塞发送的超时绝不能用 `HAL_MAX_DELAY`

成品注释的原话是“不能用 `HAL_MAX_DELAY`，否则串口异常时会永久卡死”。
所以阻塞发送要给它一个有限的超时，成品用的常数是：

```cpp
#define USART_BLOCK_TIMEOUT_MS 100U
```

### [T-75] 串口初始化时先清两个标志

成品注释的原话是“清掉上电过程中可能残留的空闲标志与溢出标志，避免刚使能中断就误触发一次”。
具体就是这两行：

```c
__HAL_UART_CLEAR_IDLEFLAG(&huart1);
__HAL_UART_CLEAR_OREFLAG(&huart1);
```

### [T-76] 接收用 `HAL_UARTEx_ReceiveToIdle_DMA`，不是 `HAL_UART_Receive_DMA`

原因在于 `Mode=NORMAL` 的 DMA 并不知道“一帧在哪里结束”（见 [T-72]）。
所以要把判断交给 IDLE 中断，也就是“线上安静下来了，说明这一帧完了”。
代价是每收完一次都要重新武装，再调一次 `USART_Receive`。

### [T-77] 参数守卫

函数一进来先检查 `huart == nullptr || pData == nullptr || Size == 0`，命中就直接返回。
不返回的话，HAL 会拿着空指针乱写。

### [T-78] `Core/Src/*.c` 的文件名就是配置清单

```
gpio.c ← C2   tim.c ← C3   usart.c ← C4   can.c ← C5
spi.c  ← C6   i2c.c ← C7   dma.c   ← C4/C5 用到的 DMA
app_threadx.c ← C9
```

所以“配一项”就等于“多一个 `.c` 文件，再改 `main.c` 和 `stm32f4xx_it.c`”。
这也意味着如果在 CubeMX 里不勾 C12 那条 `.c/.h pair`，上面这些东西会全部塞进 `main.c`（见 [T-66]）。
还有一点容易踩坑：`MX_DMA_Init()` 必须排在所有用 DMA 的外设之前。成品 `main.c` 里 `MX_DMA_Init()` 在 L96，`MX_USART1_UART_Init()` 在 L104，和 `.ioc` 的 `functionlistsort` 顺序一致，因为 DMA 时钟要先打开。

### [T-79] 成品的一个真实缺口：UART 的 `RxEventCallback` 没实现

证据是全仓搜 `RxEventCallback`，成品里只有注释提到它，实现只剩 HAL 那个 `__weak` 空函数。
后果是：`USART_Receive()` 确实把 DMA 加 IDLE 武装起来了，一帧收完之后 HAL 调用的是那个空的弱回调，于是数据进了缓冲区，却没有任何人被通知，只能靠轮询。
这不是编译错误，而是一个功能缺口。
这也是“自主维护”的第一个好机会：在 restart 里重建的时候把它补上。

### [T-80] USART1 用 `GPIO_AF7`，USART6 用 `GPIO_AF8`

两者不是同一个 AF 号。成品 `usart.c` 里写的是 `GPIO_AF7_USART1` 和 `GPIO_AF8_USART6`。
引脚复用如果 AF 号写错，一根线都不通，而且这个过程是静默的，不会有任何报错提示你。

### [T-81] `HAL_xxx_MspInit()` 做四件事，少一件就有一种故障

以 `HAL_UART_MspInit` 为例，Msp 是 MCU Support Package 的缩写。它要做的事和少做的后果对应如下：

| 步骤 | 做什么 | 少做会怎样 |
| --- | --- | --- |
| 第 1 件 | 开时钟（外设加两个 GPIO 端口）| 寄存器写了没反应，而且是静默的 |
| 第 2 件 | 把引脚配成复用（含正确的 AF 号）| 一根线都不通 |
| 第 3 件 | 建 DMA 通道并用 `__HAL_LINKDMA` 绑到句柄 | 调 `_DMA` 发送会直接返回错误，什么都不发 |
| 第 4 件 | 开外设中断（NVIC）| IDLE 进不来，[T-76] 的收帧机制永不触发 |

这里的分工要分清：`dma.c` 只管“总开关”，也就是 DMA 时钟和 NVIC；至于“哪个 Stream、哪个通道、什么方向”，全在 `usart.c` 的 `MspInit` 里。
所以 `MX_DMA_Init()` 必须排在 `MX_USART*_UART_Init()` 之前，这个顺序是由 `.ioc` 的 `functionlistsort` 决定的。

### [T-82] 分清 IntelliSense 和真编译器

面板里的报错要先看它来自谁：

| 面板里的来源 | 是谁 | 可信度 |
| --- | --- | --- |
| `C/C++(20)` | VS Code IntelliSense | 可能整片假报错，因为它的 include 路径和真编译不同 |
| `cmake GNU...` | 真编译器或链接器 | 判官 |

2026-10 实测坐实过一次：`bsp_usart.hpp` 和 `.cpp` 里 IntelliSense 报了 10 条 `uint8_t`、`__IO`、`HAL_UARTEx_*` 未定义，而真编译器一条都没报，编译全过，只在链接时报了 1 条 `undefined reference`。
所以看报错先看来源，要下结论就跑一次 `cmake --build --preset Debug`。

### [T-83] CubeMX 的 `xxx.h` 和你自己的 `bsp_xxx.hpp` 重合不等于要合并

| 文件 | 谁生成 | 放什么 | 能手改吗 |
| --- | --- | --- | --- |
| `Core/Inc/usart.h` | CubeMX | `MX_USART*_UART_Init()` 声明，加 `extern huart1` / `huart6`，加 `hdma_*` | 不能，重新生成会覆盖（见 [T-03]）|
| `bsp/include/bsp_usart.hpp` | 你自己 | 给外界用的接口：`USART_Init` / `USART_Transmit` / `USART_Receive`，加 `enum USART_Mode` | 随便改 |

它们是分层关系而不是重合关系（见 [T-08]）：`usart.h` 负责说明“有什么硬件”，也就是句柄；`bsp_usart.hpp` 负责说明“怎么用”，也就是功能。
所以 `bsp_usart.cpp` 里不要手写 `extern huart1`，改成 `#include "usart.h"` 就好。
反过来也要注意：不要把 `USART_Init()` 写进 `usart.h`，也不要把 `MX_USART1_UART_Init()` 写进 `bsp_usart.hpp`。

### [T-84] 声明与定义的区别，头文件里没有“定义”

声明是告诉编译器“有这么个东西、它长什么样”，放在头文件 `.h` 或 `.hpp` 里；定义才是真正分配内存或生成代码，放在 `.c` 或 `.cpp` 里。

以成品的 `usart.h` 为例，那个文件全文只有这些：

```c
extern UART_HandleTypeDef huart1;      /* 变量声明 —— huart1 在别处 */
void MX_USART1_UART_Init(void);        /* 函数声明 —— 实现也在别处 */
```

```c
/* usart.c 里才是定义 */
UART_HandleTypeDef huart1;             /* ← 这里才分配内存 */
void MX_USART1_UART_Init(void) { ... } /* ← 这里才生成代码 */
```

由此推出一条规则：谁用到那个变量，谁才去 include 它的头文件。

| 文件 | 要 include `usart.h` 吗 | 为什么 |
| --- | --- | --- |
| `bsp_usart.cpp` | 要 | 它用 `huart1` 和 `huart6` 这两个变量 |
| `bsp_usart.hpp` | 不要 | 它只用 `UART_HandleTypeDef *` 这个类型当参数，类型来自 HAL 头 |

这其实是好设计：函数收的是指针，而不是写死 `huart1`，于是一个函数能被两个串口共用，头文件耦合也更低。反过来如果写成 `USART1_Transmit()`，头文件就被迫 include `usart.h`，而且每个串口都要一份代码。
另外生成文件里的 `extern "C" { }` 不要删，它是 [T-68] 的解法，因为 `usart.c` 是 C 而调用方是 C++。

关于函数和变量在头文件里的写法，还有一个很实用的细分：

| 要暴露的东西 | 头文件里怎么写 | 要写 `extern` 吗 |
| --- | --- | --- |
| 函数 | `void USART_Init(void);` | 不用，函数声明默认就是 extern |
| 变量 | `extern UART_HandleTypeDef huart1;` | 必须写，不写就变成定义 |

变量必须带 `extern`，根本原因是变量不能定义在头文件里，否则每个 include 它的 `.cpp` 都会各定义一份，于是报 `multiple definition`，这正是 [T-67] 第二条讲的情况。
这样 [T-67] 和本条就闭环了：头文件只放声明，而变量的声明语法就是 `extern`。
所以“用 include 替代手写 extern”这句话的精确说法是：不是“不用 `extern`”，而是“不用你手写 `extern`”，因为 `extern` 由头文件替你写了。

### [T-85] 参考答案不等于唯一正确写法

实例是成品的 `bsp_usart.cpp` 没有 include `usart.h`，而是手写了两行：

```cpp
extern UART_HandleTypeDef huart1;   /* 内容 == usart.h L35 */
extern UART_HandleTypeDef huart6;
```

这两种写法在编译器看来完全一样，编译和链接的结果相同，所以手写不是“错”，只是“另一种写法”。
区别只在工程性上：

| 写法 | 优点 | 缺点 |
| --- | --- | --- |
| `#include "usart.h"` | 一件事只写一处，不依赖 include 顺序 | 多一层 include |
| 手写 `extern` | 少一层 include，和参考答案逐行对得上 | 重复描述，依赖 include 顺序（类型名必须先可见）|

至于“作者为什么那么写”，这件事不能乱猜。可查的线索只有：`bsp_pwm.hpp` 的 guard 是 `RM26_H7_BSP_PWM_HPP`，来自社团 H7 代码库；而 `bsp_usart.hpp` 的 guard 是 `BSP_USART_HPP`，不是同一套，所以不能一概而论。
推广一点说，成品是“能交的版本”，不是“最优版本”。所以你可以照着它对照着重写，也可以在其中一处写得比它干净，但要在 restart 里改，别动提交物。

### [T-86] `extern` 管编译期，`target_sources` 管链接期

2026-10 的实例是：`bsp_usart.cpp` 里写了 `extern huart1`，但仍然报 `undefined reference to 'USART_Init()'`，因为这是两件事。

| | `extern` 声明 | 链接器要的 |
| --- | --- | --- |
| 作用 | 告诉编译器“有这么个东西” | “真有某个 `.obj` 提供了它” |
| 时期 | 编译期 | 链接期 |
| 打比方 | 电话簿上写了“老王住这儿” | 老王真的搬进来了没有 |

关键在于符号在哪，就由哪个文件的 `.obj` 提供。

| 符号 | 谁提供 | 在 `target_sources` 里吗 |
| --- | --- | --- |
| `huart1` / `huart6` | `usart.c`（CubeMX 生成）| 在，所以没问题 |
| `USART_Init()` | `bsp_usart.cpp`（你写的）| 不在，所以链接器找不到 |

判据是看链接行，也就是 `build.ninja` 或构建输出：清单里没有那个 `.obj`，写多少 `extern` 都变不出来。
另外函数上的 `extern` 也一样没用，它同样只影响编译期。

### [T-87] 判“构建是不是最新的”：比时间戳

症状是改了源码、以为编过了，但 ELF 还是旧的，于是你看到的符号是旧代码的。
判据是：源文件的 `LastWriteTime` 比 ELF 的新，就说明构建是旧的。

```powershell
(Get-Item 源码).LastWriteTime   # 与 (Get-Item build\Debug\xxx.elf).LastWriteTime 比
```

2026-10 的实例是 `bsp_usart.cpp` 的 21:12:19 晚于 ELF 的 21:11:14，差了 1 分钟。那次改的只是注释，所以符号没错，但习惯上改完就该再编一次。
同类问题还有 [T-52]（对着工具）、[T-65]（符号在不在 ELF）、[T-69]（nm 要 demangle）。

### [T-95] 级联错误：报错的位置往往不是原因的位置

2026-10 的实例是 `bsp_can.hpp` 一行里错了两个字符：`uint8_` 少了结尾的 `t`，同时行尾缺了 `;`。面板一共报了 4 条错，横跨 2 个文件。

| 报错 | 真正的根因 |
| --- | --- |
| `bsp_can.hpp:22 identifier "uint8_t" is undefined` | `uint8_` 拼错 |
| `bsp_can.hpp:24 expected a ';'` | L22 末尾缺 `;`（编译器在 `#endif` 处才发火）|
| `bsp_can.cpp:9 expected initializer before 'void'` | 头文件坏了，残缺的声明撞进了函数定义 |
| `bsp.cpp:7 expected initializer before 'void'` | `bsp.cpp` 也 include 了同一个坏头文件 |

这里有两条铁律：第一，永远先修最上面那一条，因为下面的常常只是回声；第二，见到 `expected initializer before 'xxx'` 出现在函数定义处时，要去上一个声明那里找缺的分号，而不是去看那个函数本身。
同类问题还有 [T-67]，`multiple definition` 也是“报一处、根因在别处”。

---

## C5/C6 与 C7/C8/C9（T-96 到 T-113）

## restart 练习工程 · C6 SPI（BMI088）

### [T-96] SPI1 不在 PA5/PA6/PA7，而在 PB3/PB4/PA7

芯片的通用复用表上，`PA5/PA6/PA7` 确实标着 SPI1_SCK/MISO/MOSI，但这块板子的实际走线是 `PB3(SCK)`、`PB4(MISO)`、`PA7(MOSI)`。这两件事要分清楚：芯片能做什么是一回事，板上怎么连的又是另一回事，这正是 T-64 那条结论的第二次应验。

所以配引脚时一律以原理图为准。在 CubeMX 里要逐个点选这三个脚，别指望它自动落到 PA5/PA6/PA7 上；这一点 [C板引脚映射-已核实.md](../作业追踪/C板引脚映射-已核实.md) 的 §3.2 就写着。

### [T-97] PB3 和 PB4 默认是 JTAG 脚

```
PB3 = JTDO      PB4 = NJTRST      PA15 = JTDI
```

这是硬要求：在 `System Core` 下的 `SYS` 里，`Debug` 一项必须选 `Serial Wire`。原因是 PB3 和 PB4 默认归 JTAG 调试口，一旦在 Debug 里选了 JTAG，这两个脚就被调试口占走，SPI1 完全不工作。

这个错最麻烦的地方在于它是静默的，编译没问题，板子也不报错，现象就是读不到 ID。

### [T-98] SPI 的 NSS 必须用软件方式

这是硬要求：NSS 要用 `SPI_NSS_SOFT`，再配 GPIO 手动控制 `CS_ACC` 和 `CS_GYRO`。原因是硬件 NSS 会在每次 `HAL_SPI_Transmit` 之后自动拉高片选，而 BMI088 的读操作是"地址加连续数据"的一次突发，片选中间被拉高就把这次突发打断了。

### [T-99] BMI088 加速度计读 N 字节要读 N+1 个

加速度计读数据时，地址之后还会多吐一个无效字节，这个字节要由调用方丢弃，比如 `VerifyAccChipID` 取的就是 `pRxData[1]`。

容易踩坑的地方在于陀螺仪没有这个行为，`VerifyGyroChipID` 读 1 字节就够了，两者的读法不一样。

### [T-100] 这条板上的速度阶梯

这条板上三种总线的速度差得很清楚：SPI 是 5.25 MHz（由 APB2 的 84 MHz 除以 16 得到），远高于 CAN 的 1 Mbps，再远高于 I²C 的 400 kHz。

所以 BMI088 走 SPI 是有道理的，它需要高频采数。

## 配置项与代码的对应关系

### [T-101] 哪些配置项有你要写的代码，哪些没有

| 配置项 | 有代码吗 | 落在哪 |
| --- | --- | --- |
| **C1** 时钟 | 没有 | 全生成 |
| **C2** GPIO / 灯 | 有 | `modules/LED`（D22）|
| **C3** PWM | 有 | `bsp_pwm` |
| **C4** UART | 有 | `bsp_usart` |
| **C5** CAN | 有 | `bsp_can` |
| **C6** SPI | 没有 bsp 层 | 用法在 `modules/BMI088`（D1–D8）|
| **C7** I²C | 更不用 | IST8310 是选做，配置完就够 |
| **C8** TIM10 加热 | 没有新文件 | 用法在 `BMI088::TemperatureControl`（D8）|
| **C9 / C10** 线程 | 有 | `threads/`（T1–T4）|
| **C11 / C12 / C13** | 没有 | 纯配置 |

从表里可以看出来，真正要写代码的只有 C2、C3、C4、C5、C9、C10 这几项。所以"配一项就写对应代码"的节奏会在 C6 断一次，到那一步别以为自己漏了什么。

### [T-102] "清单"和"成品"对不上的一处实例

[C板引脚映射-已核实.md](../作业追踪/C板引脚映射-已核实.md) 的 §4 写的是"C7 I²C ＝ I2C3 ＋ 用户 I2C2（PF1/PF0）"，而成品 `.ioc` 里 `Mcu.IP` 只有 `I2C3`，`PF0/PF1` 没有任何配置，两份描述对不上。

既然成品是实际生成过的状态，就跟随成品，只配 I2C3。这也是"文档里每个肯定句都要回实际状态点一遍"的第 4 个实例。

### [T-104] 加热 20 kHz、舵机 50 Hz，用的是同一个公式

```text
频率 = TIM 时钟 / (Prescaler × Period)

TIM1 : 168M / (168 × 20000) = 50 Hz
TIM10: 168M / (168 × 50)    = 20 kHz
```

两个定时器用的是同一个 `168-1` 的 Prescaler，区别只在 `Period` 上，一个是 20000，一个是 50。

## restart 练习工程 · C8 TIM10 加热（PF6）

### [T-105] 是 GPIO_AF3_TIM10，不是 AF1

| 定时器 | AF 号 |
| --- | --- |
| TIM1 | `AF1` |
| TIM10 | `AF3` |

TIM10 要选 `GPIO_AF3_TIM10`，写成 AF1 是不对的。这和 T-80 那组 `USART1=AF7 / USART6=AF8` 属于同一类坑。

### [T-106] Pulse = 0 是安全默认

`Pulse` 默认给 0，意思是上电时加热不发力。因为加热电阻是功率器件，默认关掉比默认开着安全。`.ioc` 里没写 Pulse 时默认就是 0，这与 T-02 的说法一致。

### [T-107] 温控 PID 的输出，量纲不是占空比

```cpp
// BMI088.cpp：TempPid 的 maxOut 是 25000，量纲不是占空比，所以必须夹到 [0, 1]：
float duty = TempPid.result;
PWM_SetDutyRatio(&HEATING_RESISTANCE_TIM, duty, HEATING_RESISTANCE_CHANNEL);
```

`PWM_SetDutyRatio` 收的是 `0~1`（见 T-73），而 PID 的输出量纲不是占空比，所以这里必须夹一道。不夹的后果是 `CCR` 远超 `ARR`，输出恒高，电阻就全功率加热了。

### [T-108] PWM_Init() 要连 TIM10 一起启动

```cpp
PWM_Start(&htim1,  TIM_CHANNEL_1..4);   // 舵机
PWM_Start(&htim8,  TIM_CHANNEL_1..3);   // 舵机
PWM_Start(&htim10, TIM_CHANNEL_1);      // 加热，漏了这一行加热就不出力
```

这是硬要求：加热这一路也要在初始化里启动，漏掉 TIM10 那行的话加热就不出力。仓库 `AGENTS.md` 里记过这个坑，原话是"`PWM_Init` 也没被 `bsp_Init` 调用（加热不出力）"。

## restart 练习工程 · C9 ThreadX 中间件

### [T-109] 这两个文件里 USER CODE 一处都没有

```
tx_user.h                   : USER CODE  0 处
tx_initialize_low_level.s   : USER CODE  0 处
app_threadx.c               : USER CODE 11 处
```

上面是实测数据。`tx_user.h` 和 `tx_initialize_low_level.s` 里一个 `USER CODE` 标记都没有，而同目录的 `app_threadx.c` 有 11 处，所以重新生成时前两个文件会被整文件覆盖，tick 也跟着从 1000 掉回 100。

它们不是 CubeMX 的通用模板，而是 pack 的 `.ftl` 生成的，这一点已经实锤，路径如下：

```
…\Repository\Packs\STMicroelectronics\X-CUBE-AZRTOS-F4\1.1.0\STM32CubeMX\templates\
    tx_user_h.ftl · tx_initialize_low_level_s.ftl
```

### [T-110] 启动顺序：bsp_Init() 在 MX_ThreadX_Init() 之前

这是硬要求：`bsp_Init()` 要放在 `MX_ThreadX_Init()` 前面。成品 `main.c` 注释的原话是"必须在 `MX_ThreadX_Init()` 之前：线程一创建就会用到 CAN 与 PWM"。

原因也在这里：线程一起来就要发 CAN、开 PWM，如果外设还没起来，线程跑起来也是废的。

### [T-111] 覆盖问题不能自动修，只能让它会响

| 想法 | 为什么不通 |
| --- | --- |
| 给那两个文件加 `USER CODE` | 模板来自 pack 的 `.ftl`，改模板的话它不在仓库里，升级就没了，还会影响全机器 |
| 把值搬去 `CMakeLists` | 它确实能扛过生成，但两个值的来源不同，只回退一个反而制造新的不一致 |
| 编译选项 `-DTX_TIMER_TICKS_PER_SECOND=1000` | `tx_user.h` 里是无条件 `#define`，会重定义冲突 |

目前能做的有两条，而且两个条件都已经具备。一是这两个文件都被 git 跟踪，所以 `git diff -- board/` 一定能发现改动；二是加一个检查脚本（例如 `测试/cubemx-tick.ps1`），让它去读两处的值、核对算法一致性，再反汇编核对 167999。

这样一来就把静默错误变成了会响的检查（见 T-51），这属于流程修复，不是代码修复。终极兜底是用秒表量 `HAL_Delay(10000)`，这是唯一端到端的验证手段。

这件事已经落地，脚本在 [测试/cubemx-tick.ps1](../测试/cubemx-tick.ps1)，它检查五件事：头文件里的值、`.s` 里每一处除数、两者是否一致、`app_threadx.c` 的 `USER CODE`，以及可选的反汇编 167999。故障注入也验证过了：拿 `restart/guoqing` 跑，它准确报出 2 条 FAIL，分别是头文件被注释成 100 和 `.s` 的除数是 100。

### [T-112] "勾选"不等于"实例化"（Software Packs 中间件）

现象是这样的：在 `Software Packs` 下的 `Select Components` 里勾了 `X-CUBE-AZRTOS-F4` 的 `RTOS/ThreadX/Core` 并按了 OK，结果一个文件都没生成，`Middlewares/`、`AZURE_RTOS/`、`app_threadx.c`、`tx_user.h` 全都没有。

`.ioc` 里的判据是这样的：

| 键 | 含义 |
| --- | --- |
| `Mcu.ThirdParty0=…` 加 `…_SwParameter=…:true;` | 表示"我勾选了什么"，也就是选择 |
| `….IPParameters=…` 加 `…ThreadXCcRTOSJjThreadXJjCore=true` 加 `VP_….Mode`/`.Signal`，以及多出来的 `Mcu.Pin*` | 表示"它真的被加进工程了"，也就是实例化 |

只有 `_SwParameter` 而没有后面这几种，就说明是勾了但没加进去。修法是回到 `Pinout & Configuration`，左边这时应该出现 `Middleware and Software Packs` 那一组；如果看不到，就是没生效。在里面找到 Mode 面板，选中 Core，再生成就可以了。

注意不要手改 `.ioc` 去补键，因为 GUI 会按自己的模型重写；也不要改 pack 的 `.ftl`。

### [T-113] pack 的默认 tick 就是 100 Hz

`X-CUBE-AZRTOS-F4` 生成的两个文件，默认值直接就是 100 Hz：

```
tx_user.h : /*#define TX_TIMER_TICKS_PER_SECOND                100*/   这一行本来就是注释状态
low_level : SYSTICK_CYCLES = ((SYSTEM_CLOCK / 100) -1)                除数是 100，算出 1679999
```

也就是说，成品里的 `1000` 和 `/1000` 是手工改出来的，不是默认值。这一点容易踩坑的地方在于，第一次生成就要改，而且每次重新生成都要再改一遍（见 T-109 和 T-111）。

---

## PID、threads、git 与 BMI088（T-114 到 T-134）

## restart 练习工程 · `libs` 的 PID（`pid.cpp` 49 行）

### [T-114] 先"移窗"、再"算新误差"，顺序不能反

```cpp
err[2] = err[1];      // 上一拍的 e1 → e2
err[1] = err[0];      // 上一拍的 e0 → e1
err[0] = ref - fdb;   // 本拍的新误差
```

这三行的顺序是硬要求，必须先把历史误差往后挪一格，最后才算本拍的新误差。

因为 `err[0]` 一旦被新值覆盖，上一拍的 `e0` 就再也找不回来了。顺序写反的话，微分项永远用错的历史值。

而这一点容易踩坑的地方在于它不报错、不崩溃，只有曲线不对。

### [T-115] 位置式：`iResult` 是累加器，必须单独限幅

```cpp
iResult += ki * err[0];
iResult = Numeric::LimitABS(iResult, maxIOut);                     // 积分限幅
result  = Numeric::LimitABS(pResult + iResult + dResult, maxOut);  // 输出限幅
```

位置式里的 `iResult` 是个累加器，所以必须单独限幅。

如果只限输出、不限积分，误差长时间偏在一边时 `iResult` 会涨到天上，等误差反号以后要很久才降下来，结果就是积分饱和和超调。

两个限幅管的事不一样，`maxIOut` 管的是内部状态，`maxOut` 管的是对外命令。

### [T-116] 增量式没有积分累加器，天然不饱和

```cpp
pResult = kp * (err[0] - err[1]);
iResult = ki * err[0];
dResult = kd * (err[0] - 2.0f*err[1] + err[2]);
result  = Numeric::LimitABS(result + pResult + iResult + dResult, maxOut);
```

增量式里没有积分累加器，所以它天然不饱和。

这一点容易踩坑的地方在于，两种模式里 `pResult`、`iResult`、`dResult` 的含义完全不同：位置式里它们是绝对量，增量式里它们是本次增量。

所以两套代码看着长得像，却不能互相照搬，判断某一行是对是错之前得先确认自己在哪套模式里。

### [T-117] `LimitABS` 是对称限幅，而写错的后果比不写更危险

```cpp
if (input >  maxValue) return  maxValue;
if (input < -maxValue) return -maxValue;   // ★ 这个负号不能少 ✗
return input;
```

`LimitABS` 是对称限幅，下界那个负号是它的全部要害。

如果把判断写成 `input < maxValue`，也就是少掉那个负号，那么任何正常值都会被判成越界并返回 `-maxValue`，输出整体反号，电机反向满转，这比干脆不限幅危险得多。

实测结果是 pid 套件 20 条里 15 条红，`got` 恒等于 `-maxOut`，而编译完全通过。对比一下，`bsp_pwm` 的 `PWM_SetDutyRatio` 是 `[0,1]` 非对称限幅，两种限幅别混着用。

---

## restart 练习工程 · `threads` 的 alive 线程

### [T-119] `Threads_CreateAll` 必须定义在 `namespace` 外面

这是硬要求：`Threads_CreateAll` 必须定义在 `namespace` 外面。

因为它在 `threads.hpp` 里是用 `extern "C"` 包的，而 C 链接的函数不能待在 C++ 命名空间里，写进去就会报 `undefined reference to Threads_CreateAll`。

同理，`namespace Threads { … }` 的右括号必须放在所有函数之后、`Threads_CreateAll` 之前。常见错法是在信号量定义之后就顺手把 `}` 写上，后面的 `Threads::CreateAliveThread()` 就全都找不到了。

### [T-120] 信号量：`initial_count` 取 0，每个都查返回值

```cpp
status = tx_semaphore_create(&Threads::sem_init_done, (CHAR*)"init done", 0U);
if (status != TX_SUCCESS) { return status; }
```

信号量的 `initial_count` 要取 0，理由是事件由被监控者 `put` 产生，不要预先置数。

如果预置成非零，alive 线程一上电就会以为已经有人报到过了。

每个信号量创建完都要查返回值，失败就直接把状态返回去，别接着往下跑。

### [T-121] `tx_thread_create` 的 10 个参数

```cpp
tx_thread_create(&thread,          // 1  控制块
                 (CHAR*)"alive",   // 2  名字（调试器可见）
                 AliveThreadEntry, // 3  入口
                 0,                // 4  入口参数
                 stack,            // 5  栈起始
                 sizeof(stack),    // 6  栈字节数
                 priority,         // 7  优先级
                 priority,         // 8  抢占阈值（= 优先级 ⇒ 同级不抢占）
                 TX_NO_TIME_SLICE, // 9  关闭时间片轮转
                 TX_AUTO_START);   // 10 创建后立即启动
```

这十个参数依次是控制块、名字、入口、入口参数、栈起始、栈字节数、优先级、抢占阈值、时间片设置、启动方式。

名字那一项只给调试器看，所以不影响运行。

成品把优先级传了两次，这一点容易踩坑的地方在于它看起来像笔误，其实不是：第 8 个是抢占阈值，取成和优先级相同，效果是同级线程之间不互相抢占。

### [T-122] `while(true)` 里必须 sleep，线程永不返回

线程主体里有两个硬要求：`while(true)` 里必须 sleep，而且线程永不返回。

不 sleep 会独占 CPU，别的线程根本没机会跑；return 则会让 ThreadX 报错。

`LED::Init()` 这种初始化要放在循环之前，给线程一个确定的起点。

### [T-123] tick 有两道保险，第二道是编译期的

`threads.hpp` 里：

```cpp
static_assert(TX_TIMER_TICKS_PER_SECOND >= 1000U,
              "控制周期取 1 ms，要求 ThreadX tick 频率不低于 1 kHz（见 Core/Inc/tx_user.h）");
```

tick 有两道保险，第一道是 `-Fix` 和 `git diff` 保证值写成 1000，第二道是这条 `static_assert`，它在编译期保证这个值真的是 1000。这两道管的事不一样，所以第二道不能省。

如果 tick 被打回 100，那么 `MsToTicks(1)` 会算成 0，控制线程就变成"不 sleep 地空转"。

实测中这条断言确实触发过，报错信息是 `the comparison reduces to '(100 >= 1000)'`，编译直接失败。

### [T-124] 未跟踪的生成文件，`git diff` 看不到它

C9 的对策是"生成后跑一次 `git diff` 就能发现 tick 被打回"，但这个对策成立的前提是文件已经被跟踪，这一点容易踩坑的地方就在这里。

实测发现，`restart` 里的 `tx_user.h` 和 `tx_initialize_low_level.s` 当时是 `??` 状态，也就是未跟踪，所以 `git diff` 什么都不显示。

这不是 `.gitignore` 忽略了它们，只是从来没 add 过。

对策是先 `git add` 一次，再动手改，这样改完 `git diff` 才看得见。

---

## `modules` / BMI088

### [T-125] 寄存器写对了不等于芯片进入了那个状态

加速度计要出数据，三步缺一不可，这是 `BMI088.cpp` 原注释里就写着的：

```
① PWR_CTRL = 0x04（上电）
② PWR_CONF = 0x00（active；0x03 是 suspend）
③ ACC_CONF 的 ODR 有效（0xAB = 800 Hz / normal）
```

顺序也要按 Bosch 官方 `bmi08a_set_power_mode()` 来：`PWR_CONF=0x00`，等 5 ms，再 `PWR_CTRL=0x04`，再等 5 ms。因为 enable 是在芯片仍然 suspend 的时候写下的，写反了就不作数。

写反的后果很容易骗人：三个配置寄存器读回全部正确，`0x7D=0x04`、`0x7C=0x00`、`0x40=0xAB`，而数据寄存器恒为 0。真机现象就是 acc 全 0、温度 22.4 °C、陀螺仪正常。

所以判据是：确认配置生效不能只看读回值，要看芯片有没有真的产出数据。

官方还有两件容易漏的事，软复位后要补一次 dummy SPI 读，写完 `ACC_CONF` 之后要等 40 ms。

### [T-126] 虚函数会让"符号在不在 ELF 里"这个判据失效

`VerifyAccChipID()` 和 `VerifyGyroChipID()` 是虚函数，它们通过 vtable 被引用，所以符号永远待在 `.elf` 里。这就让 `nm` 和 `reachability` 那套"不在 ELF 里就是不可达"的判据查不出问题。

这里还叠加了第二个问题：`IMU.hpp` 里 `INIT_ERR` 默认就是 `true`，而全项目在修复前只有把它置 `true` 的地方，没有置 `false` 的地方。修复后 `BMI088.cpp` 的 L101 和 L148 两个成功分支补上了 `= false`，写点时统计是 `true` 八处加 `false` 两处。

这两件事合起来的后果是 imu 线程报告初始化失败，真机第一次上电就是红灯常亮，跟 BMI088 芯片本身好坏无关。

所以判据要问两遍：第一问符号在不在 ELF 里，这能抓出非虚函数的死代码；第二问有没有真实调用点，虚函数只能靠全仓库搜索。

修复的做法是在 `imuthread.cpp` 补 2 行调用，再在两处成功分支补 `INIT_ERR = false`。

### [T-127] 灵敏度常数用头文件里的，不要自己算

`IMU_ACCEL_6G_SEN = 0.00179443359375f`，`IMU_GYRO_2000_SEN = 0.001065264436031695…f`。

这是硬要求：灵敏度常数一律用头文件里这两个值，不要自己推。

原因是它们是验证过的值，56 条主机断言的期望值就是照着它们算出来的。

自己推出来的近似值会让断言变红，而差值很小，肉眼根本看不出来。

### [T-128] 错误标志初值取 `true`，"默认不安全"（fail-safe）

`IMU.hpp` 的 `imu_error_t`：

```cpp
bool ACC_CHIP_ID_ERR = true;    // 加速度计ID错误则为true
bool ACC_DATA_ERR    = true;
bool GYRO_CHIP_ID_ERR= true;
bool GYRO_DATA_ERR   = true;
bool INIT_ERR        = true;
bool CALIBRATE_ERR   = false;   // ★ 只有它是 false
bool TEMP_CTRL_ERR   = true;
```

这一组错误标志的初值取 `true`，含义是"默认认为有错"，只有代码显式改成 `false` 才算通过。

这样设计以后，漏写一次 `= false` 的后果是偏保守地报错，而不是偏激进地假装正常。如果初值取 `false`，忘了跑某个检查时标志还停在 `false`，系统就会认为一切正常，然后带着没初始化的传感器跑控制环。所以这里的取舍是宁可误报，不可漏报。

`CALIBRATE_ERR = false` 是唯一的例外，因为它一定会被写到：`BMI088.cpp` 的 L49 在标定异常时写 `true`，L57 在标定正常时写 `false`，`Calibrate()` 的 if 和 else 两条路都写它，不存在漏写。

别的标志不能这么办，`TEMP_CTRL_ERR` 只在 `TemperatureControl()` 里写，`*_DATA_ERR` 只在 `VerifyXxxData()` 里写，没调用就等于没写过。

由此得到一条通用判据：看这个标志有没有一定会被执行的写入点。有的话初值随便取，`CALIBRATE_ERR` 就是这种；没有、只在特定路径写的话，初值必须取安全的那个，其余六个都属于这种。所以 T-126 的修复是补写点，不是把初值改成 `false`，改初值会把"默认安全"变成"默认危险"。

### [T-129] "总闸"会被后面的成功覆盖，清零点要放对

`INIT_ERR` 是个总闸，它同时代表 ID 校验、三个配置寄存器回读和 SPI 传输三件事，而每个检查都写它，所以后面的成功会盖掉前面的失败。

成品 `BMI088.cpp` 的 L103 注释把话说得很直白："必须放在清 `INIT_ERR` 之后，否则读回失败会被后面的成功覆盖掉"。

成品的处理分两层：同一个函数内部的多个检查只写 `if (...) { INIT_ERR = true; }` 置位，不做整体清零，见 L114、L120、L127；清零则只在函数开头那个检查通过之后做一次，见 L98 到 L101。

推论是：多个检查共用一个总闸时，必须先清、再逐项只置位。反过来的话，就等于"最后一项成功等于全部成功"。

---

## restart 练习工程 · `BMI088.cpp` 与 `imuthread.cpp`

### [T-130] `else` 分支里把 `INIT_ERR` 置成 `true`（成功也判失败）

```cpp
if (HAL_SPI_Receive(...) != HAL_OK) { self_test.INIT_ERR = true; }   // 失败 ⇒ true ✓
else                                { self_test.INIT_ERR = true; }   // ✗✗ 成功也 true
```

这里两个分支都写了 `true`，也就是成功也判失败。

结果是读成功也被判成初始化失败，上电永远红灯常亮。

`else` 那一支应该是 `false`，或者干脆不写，这正是 T-128 那个模式的直接应用。

### [T-131] `SelectCS(cs, GPIO_PIN_SET)` 被写进 `if` 里面，失败时片选永不释放

```cpp
SelectCS(cs, GPIO_PIN_RESET);
if (HAL_SPI_Transmit(...) == HAL_OK)
{
    ...
    SelectCS(cs, GPIO_PIN_SET);      // ✗ 在 if 里面
}                                    //   传输失败 ⇒ 器件一直被选中 ✗✓
```

片选释放那一句被写进了 `if` 里面，所以只有传输成功才会释放。

传输失败时器件一直被选中，一次失败之后总线上所有 SPI 全乱。

所以 `SelectCS(SET)` 必须放在 `if / else` 之后，照成品的结构写。

### [T-132] `ReadXxxData(&local)` 写局部，`VerifyXxxData()` 读成员，必须先拷回成员

```cpp
acc_data_t acc = {};
s_imu.ReadAccData(&acc);        // ← 写你给的指针（局部 ✓）
s_imu.acc_data = acc;           // ★ 这一行不能少 ✗✓
s_imu.VerifyAccData();          // ← 无参数 ⇒ 只能读 this->acc_data（成员 ✓）
```

`ReadXxxData(&local)` 写的是你传进去的那个局部变量，而 `VerifyXxxData()` 没有参数，只能读成员，所以中间必须先拷回成员。

漏掉中间那行的话，校验的永远是上一帧的数据。第一拍更糟，校验的是零初始化的成员，`< 1.0` 那条判断会把好数据判成坏的。

这一点容易踩坑的地方在于它不报错、不崩溃，只有 `valid` 字段一直停在 `false`。

### [T-133] `s_imu` 必须是静态存储

```cpp
/* IMU 对象放静态存储：标定得到的陀螺零偏要一直保留，不能被重建 */
static BMI088::cBMI088 s_imu;
```

`s_imu` 必须是静态存储，因为零偏是 `Calibrate()` 算出来的状态，重建对象就等于前功尽弃。

而且放静态存储不占线程栈，imu 的栈只有 1536 B。

同理 `s_sample = {}` 必须清零初始化，否则 `valid` 会是随机值。

### [T-134] 注释掉的声明等于不存在，而 grep 会把注释里的同名文本也算进去

`threads.hpp` 里的实例：

```cpp
void AliveThreadEntry(ULONG input);
// void ImuThreadEntry(ULONG input);        ← ★ 被注释掉了 ✗✗
// void ControlThreadEntry(ULONG input);
```

被注释掉的声明等于不存在。这会编译报 `'ImuThreadEntry' was not declared in this scope; did you mean 'AliveThreadEntry'?`，因为 `CreateImuThread()` 在 L16 就用了 `ImuThreadEntry`，而它在 L24 才定义，C++ 要求先声明后使用，头文件就是这个声明该待的地方。

排查时的坑在于用 `-match` 搜 `ImuThreadEntry` 会匹配到注释里那一行，从而误判成声明存在。所以判据是：搜到的行要确认它在注释外面。

另一侧要知道，只声明不定义是安全的，因为声明不产生引用，真正会炸的是调用。

所以 `CreateControlThread()` 必须继续注释着，直到 `controlthread.cpp` 写出来。

---

## DJIMotor/M2006/Handler/回调/control/栈（T-135 到 T-156）

## modules / DJIMotor（基类）与 M2006

### [T-135] 串级里两个环的 maxOut 量纲不同

DJIMotor.hpp 的原注释写得很清楚：下面两组参数是 M2006 台架实测得到的默认值，顺序是 kp / ki / kd / maxOut / maxIOut。其中速度环的输出直接就是电流指令，所以它的 maxOut 是电流上限；而位置环的输出是速度目标，单位是 rad/s，串级的时候这个输出会喂给速度环。

```cpp
PID speedPid    = PID(150.0f, 0.0f, 0.0f, 5000.0f, 1000.0f, PID_POSITION);
PID positionPid = PID(150.0f, 0.0f, 0.2f, 5000.0f, 1000.0f, PID_POSITION);
//                     kp     ki    kd    maxOut   maxIOut
```

这正是 [T-115] 那条结论的实物：maxOut 和 maxIOut 必须是这个环输出的那个物理量的合理范围。串级里两个环输出的量纲完全不同，一个是电流一个是速度，所以两套限幅必须分别设定，不能套用同一个数。

### [T-136] last_ecd 和 ecd 成对是为了处理编码器回绕

last_ecd 和 ecd 这两者要成对保存，目的就是给编码器回绕留出计算差值的余地。这一点容易踩坑的地方在于差分的类型：差分必须用 int16_t 这种有符号类型，如果用 uint16_t 相减再当成有符号数来用，结果就会出错。

仓库的坑表里对这件事的原话是，编码器回绕要 wrap 而不要 clamp，8191 到 0 应该算成加 1，而不是减 8191。

### [T-137] AliveFlag 和 Pre_AliveFlag 是在线检测的两个计数

AliveFlag 和 Pre_AliveFlag 是一对用来做在线检测的计数器，思路很简单：CAN 反馈帧每来一帧，AliveFlag 就自增一次，然后周期性地把这两个值拿来比较。

如果比较下来两个值没有变化，就说明这一段时间里没有新帧到达，电机已经掉线，于是状态被置成 MOTOR_OFFLINE。反过来，只要两个值不相等，就说明帧还在来，电机在线。

### [T-138] GearBox_XRoll = 3 是仓库“占位值”故事的现场

仓库里记着这样一段经过：GearBox_XRoll 曾经填的是 1.0，旁边还加了注释“待查手册”。后来真值查到了，是 268/17，而且在检测题里也讨论过，但是没有人回头去改代码；再往后，文档里就被写成了“现已补全”。

这件事的教训是，只带一条注释的占位值会被人当成已经完成，其中也包括未来的你自己。AGENTS.md 第 4 节讲的就是这个情况。

### [T-139] RELAX_MODE 必须把两个 PID 的历史一起清掉

```cpp
case RELAX_MODE:
    // 松开模式必须把 PID 的历史一起清掉，否则从松开切回闭环时，
    // 积分项还带着旧的累积值，会先冲一下再收敛
    speedPid.Clear();
    positionPid.Clear();
    currentSet = 0;
```

如果只把 currentSet 置成 0 而不清 PID 的历史，那么从松开模式切回闭环的时候，电机就会先冲一下再收敛。这里最坑的一点是它不会报任何错，只能靠现象去发现。

### [T-140] 限幅有两道，量纲不同

```
第一道 PID 内部:  speedPid.maxOut = 5000                这台电机的台架实测值
第二道 setOutput: LimitABS(result, maxCurrent=10000)     手册的允许电流
```

限幅一共有两道，而且两道的量纲不一样。第一道在 PID 内部，speedPid.maxOut 等于 5000，这是这台电机台架实测出来的值，作用是防止积分项和输出跑飞；第二道在 setOutput 里，用 LimitABS 把结果限在 maxCurrent 等于 10000，这是手册允许的电流。

两道都必须保留。另外，maxCurrent 是 10000，比 maxOut 的 5000 大，所以平时它根本不起作用，它是最后一道保险。

### [T-141] 串级：位置环的输出直接当速度环的 ref

```cpp
positionPid.ref = positionSet;   positionPid.fdb = motorFeedback.positionFdb;  positionPid.UpdateResult();
speedPid.ref    = positionPid.result;        // 位置环输出就是速度环目标（rad/s）
speedPid.fdb    = motorFeedback.speedFdb;    speedPid.UpdateResult();
currentSet = (int16_t)Numeric::LimitABS(speedPid.result, (float)maxCurrent);
```

串级的接法就是位置环的输出直接当作速度环的 ref，中间不做任何换算。两个环用的是同一个 PID 类，但语义完全不同：一个输出的是电流，另一个输出的是速度。

### [T-142] AliveCheck 里 Pre_AliveFlag 只在“在线”那一支更新

```cpp
if (AliveFlag == Pre_AliveFlag) { MotorState = MOTOR_OFFLINE; }   // 这一支不更新 Pre_AliveFlag
else { Pre_AliveFlag = AliveFlag; MotorState = MOTOR_ONLINE; }    // 这一支才更新 Pre_AliveFlag
```

AliveCheck 的关键细节在于，Pre_AliveFlag 只在判定为在线的那一支里更新。如果两支都去更新它，那么下一次比较时两个值永远相等或者永远不等，电机就会被永远判成在线，掉线检测会静默失效。

### [T-143] maxCurrent = 10000 的来源是手册

M2006.cpp 的原注释写着，M2006 手册规定，允许电流对应反馈帧里的原始值上限就是 10000。

这是仓库规范里“硬件事实必须查证”的一个实例。这个数不能凭感觉填，它对应的是手册给出的电流上限，改动它等于改动电机的允许工作范围。

---

## modules / DJIMotorHandler

### [T-144] DJI 电机是大端，BMI088 是小端，两者相反

DJI 的反馈帧布局在 DJIMotorHandler.cpp 的原注释里有记录：

```
[0..1] 编码器     [2..3] 转子转速 rpm     [4..5] 实际电流     [6] 温度     [7] 保留
全部是大端，也就是高字节在前，解析式是 (data[0] << 8) | data[1]
```

发控制帧的时候也是大端：

```cpp
frames[bus][half][slot * 2U]     = (uint8_t)(current >> 8);    // 高字节在前
frames[bus][half][slot * 2U + 1U] = (uint8_t)(current & 0xFFU);
```

而 BMI088 是小端，这一点在 [T-99] 和 BMI088 的 ReadAccData 里都提到过。也就是说，同一块板子上两套字节序是相反的，这一点很容易搞混。

仓库坑表的原话是，手工移位并注释协议，不要用 memcpy。

### [T-145] 编码器回绕要 wrap，不要 clamp

```cpp
int16_t delta = (int16_t)(fb.ecd - fb.last_ecd);          // 先取有符号差
if (delta >  kEncoderHalfTicks) { delta -= kEncoderTicks; }  // +8191 修正成 −1
else if (delta < -kEncoderHalfTicks) { delta += kEncoderTicks; }
```

回绕必须按环绕处理，不能按钳位处理。8191 变到 0，正确的增量是加 1，而不是减 8191。如果做钳位，位置就会在每次回绕的时候跳半圈。另外差分必须用 int16_t，用 uint16_t 相减再当有符号用会出错。

涉及的参数有两个：kEncoderTicks 等于 8192，对应 13 位转子编码器；kEncoderHalfTicks 等于 4096。

### [T-146] Meyers singleton：Instance() 里放函数内静态对象

```cpp
static DJIMotorHandler *Instance()
{
    static DJIMotorHandler instance;    // 函数内静态，首次调用时构造
    return &instance;
}
```

单例的写法是在 Instance() 里放一个函数内的静态对象。从 C++11 起，函数内静态变量的初始化是线程安全的，这一点由标准保证。

这样做不需要 new 和 delete，不会内存泄漏，也不会遇到静态初始化顺序的问题。

要提醒的是，它在你 restart 里还牵出一个接线问题，见 [T-148]。

### [T-147] 只发“含有已注册电机”的帧

```cpp
bool CAN1_0x200_Exist, CAN1_0x1FF_Exist, CAN2_0x200_Exist, CAN2_0x1FF_Exist;
...
if (frame_exist[bus][half]) { CAN_Transmit(bus_handle[bus], frame_id[half], frames[bus][half], 8U); }
frame_id[2] = {0x200U, 0x1FFU};
```

槽位的分配是固定的：0 到 3 由 0x200 驱动，4 到 7 由 0x1FF 驱动，registerMotor 里就是按 index 小于 4 来置标志的。

发送的时候只发含有已注册电机的那一帧，没有电机的帧不上总线。否则总线上全是无意义的空帧，既浪费带宽又占用 CAN 邮箱。至于帧里未注册的槽位，一律补 0，写法是 (motor == nullptr) ? 0U : motor->currentSet。

### [T-148] restart 里缺“收帧到解析”的这一根线

成品是这么接的：在 bsp_can.cpp 的 HAL_CAN_RxFifo0MsgPendingCallback 里，直接调用 DJIMotorHandler::Instance()->updateFeedback(...)。这就是 BSP 层反向依赖 modules 层。

而在你 restart 里，那一行是被注释掉的，你自己的注释写的是“BSP 层只管收数据，不管数据怎么解析，解析交给专门的业务层”。从架构上看这样更干净，但那就必须补一个替代机制，否则 updateFeedback() 永远不会被调用，AliveFlag 永不增加，所有电机都会被判成 MOTOR_OFFLINE，而且不报错。

补法有三种：

| 做法 | 说明 |
| 写法一 回调注册（推荐）| bsp_can 暴露 CAN_SetRxCallback(fn)，业务层在初始化时注册，BSP 不认识 modules |
| 写法二 弱符号 / 钩子 | bsp_can 声明 __weak void CAN_RxHook(...)，modules 覆写它 |
| 写法三 放回 bsp_can | 最简单，但恢复了反向依赖，成品就是这条 |

判断的依据是 updateFeedback 是不是虚函数，它不是，所以用 nm 或者 reachability 能查出“没人调用它”，这也是 [T-126] 的对照面。

### [T-149] 回调注册：BSP 暴露函数指针，业务层注册，依赖单向

做法的目的是让 bsp_can 不认识 modules，也就是它不去 #include "DJIMotorHandler.hpp"。

```cpp
/* bsp/include/bsp_can.hpp */
typedef void (*CAN_RxCallback_t)(CAN_HandleTypeDef *hcan, uint32_t stdId, uint8_t *data);
void CAN_SetRxCallback(CAN_RxCallback_t cb);

/* bsp/src/bsp_can.cpp */
namespace { CAN_RxCallback_t s_rxCallback = nullptr; }      // 文件内静态
void CAN_SetRxCallback(CAN_RxCallback_t cb) { s_rxCallback = cb; }
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    ...读 rxHeader / rxData...
    if (s_rxCallback != nullptr) { s_rxCallback(hcan, rxHeader.StdId, rxData); }   // 只转发，不做解析
}

/* modules/src/DJIMotorHandler.cpp */
namespace {
void OnCanRx(CAN_HandleTypeDef *hcan, uint32_t stdId, uint8_t *data)
{
    if (stdId < 0x201U || stdId > 0x208U) { return; }
    DJIMotorHandler::Instance()->updateFeedback(hcan, data, (int)(stdId - 0x201U));
}
}
void DJIMotorHandler_Init(void) { CAN_SetRxCallback(OnCanRx); }
```

这样依赖方向就是 modules 指向 bsp，单向的，bsp 里只有一个函数指针。

还有一点必须记住：回调是在中断上下文里执行的，所以里面只能做“解析加写变量”这件事。绝不能调用 tx_semaphore_put、任何 tx_ 开头的函数、HAL_Delay、阻塞操作或者 printf。仓库里那条规范说的是“没有任何 ISR 调用 tx_”。

下面三条必须同时做到，少一条就静默失效。第一，DJIMotorHandler_Init 要在 .hpp 里声明，只定义不声明就等于没人能调它。第二，要有人真的调它，推荐放在 Threads_CreateAll() 里、建线程之前，否则又是 [T-64] 那个“没人调用”的毛病。第三，ISR 里只留回调，不要同时保留直接调用，两条都留的话每帧会被处理两次，而且反向依赖又回来了。

第四个副作用也要注意：DJIMotorHandler.cpp 一旦引用 CAN_SetRxCallback，主机测试就会链接失败，因为主机端不链 bsp_can.cpp。对策是给主机测试补一个桩，测试/shim/bsp_can_stub.cpp 提供空实现；桩里用裸签名声明，不用 CAN_RxCallback_t，因为成品没有那个 typedef。

和“放回 bsp_can”相比，这种做法的好处是依赖单向、可以换假回调，代价是多了一个“忘注册”的失败模式，这个只能靠 nm 兜住。

---

## 工具链本身的两个坑（测试/ 的脚本）

### [T-150] 子进程输出乱码，Select-String 匹配不到，汇总变成 0

现象是这样的，run.ps1 实测过：

```
每一行测试都是 PASS    PASS=42 / 27 / 28 都打出来了
而脚本报： NO SUMMARY LINE     TOTAL PASS=0 FAIL=0   suites with problems: 5
```

根因在于 Windows PowerShell 5.1 按旧 ANSI 码页去解码子进程的 UTF-8 输出，于是汇总行的中文前缀“==== 合计：PASS=”变成了乱码，而乱码产生的字节把紧跟其后的那个 P 吃掉了，只剩下“锛�ASS=”，所以 Select-String 'PASS=' 永远匹配不到。

那为什么“我这边是好的”呢？因为我的运行环境本身就是 UTF-8，这个故障只在用户自己开的窗口里复现。

修法是脚本开头加一行：

```powershell
[Console]::OutputEncoding = [Text.Encoding]::UTF8
```

验证方式要用故障注入，不能只说“我这边是好的”：

```powershell
[Console]::OutputEncoding = [Text.Encoding]::GetEncoding(936)   # 先把窗口弄坏
& run.ps1 -Repo <...>                                           # 脚本应自己纠正回来
```

实测结果是乱码消失、PASS=173 / FAIL=0、跑完后 OutputEncoding 变成 utf-8。

由此还得出一条判断：如果每一行都 PASS 但汇总是 0，先怀疑输出编码，不要去改测试。

最后提醒一点，这一条只对输出中文的脚本有意义。reachability、stackdepth、progress、cubemx-tick 输出的全是 ASCII，没有这个风险。

---

## threads / controlthread（最后一个手写文件）

### [T-151] 控制环一拍的四个步骤顺序

```cpp
while (true)
{
    s_m2006.setOutput();                 // 1. 用本帧之前收到的反馈算电流
    s_handler->sendControlData();        // 2. 上总线
    s_handler->AllMotorAliveCheck();     // 3. 检查所有注册电机的在线状态
    ...第 4 步用 MotorState...            // 4. 读第 3 步刚写的结果
    (void)tx_semaphore_put(&sem_control_beat);
    tx_thread_sleep(kControlPeriodTicks);   // 1 ms
}
```

四个步骤的顺序不能乱。第一步用这一帧之前收到的反馈算电流，第二步把数据发上总线，第三步检查所有注册电机的在线状态，第四步才去读第三步刚写下的结果。也就是说，AliveCheck 必须在它被读之前、而且在发之后。

这样安排的结果是第四步的反应有一拍延迟，这个延迟是可以接受的，因为一拍只有 1 ms。

### [T-152] 掉线就强制松开（全篇最重要的一行）

```cpp
if (s_m2006.MotorState != M2006::MOTOR_ONLINE) { s_m2006.controlMode = M2006::RELAX_MODE; }
else                                           { s_m2006.controlMode = g_controlMode; }
// 掉线就松开：不能拿着过期的反馈继续做闭环
```

这一行之所以致命，是因为反馈一旦停了，speedFdb 和 positionFdb 就会冻结在旧值上，PID 以为“还没到”，输出就会一直加大，最后飞车。

另外，RELAX_MODE 里 setOutput() 会 Clear() 两个 PID（见 [T-139]），所以重新上线的时候不会“先冲一下”。这两个坑在这里正好串起来了。

### [T-153] 用 volatile 调试开关，在线改模式不用重烧

```cpp
/* 调试开关：控制模式放在这里，便于按 V3 的要求分别验证三种模式。
 * 默认 RELAX_MODE —— 上电不出力，这是第一次上电唯一安全的默认值。
 * 用调试器改这个变量即可切换：0 = RELAX、1 = SPD、2 = POS。 */
static volatile M2006::MotorControlModeType g_controlMode = M2006::RELAX_MODE;
```

这就是“怎么分别验证三种模式”的答案：用调试器改变量，不重编也不重烧。

这里的 volatile 不能少，否则编译器可能把这个变量优化掉，调试器改了也不生效。

### [T-154] positionSet = 0.0f 的含义

原注释是这样写的：位置模式走的是“位置环套速度环”，也就是题目说的双环。positionSet 给 0，意思是位置模式下保持住开机时输出轴所在的位置。

之所以能这么理解，是因为 positionFdb 是从 0 开始累加的，所以 positionSet 等于 0 时，目标就是上电那一刻的那一点。

也就是说，上电时输出轴停在哪儿，它就守在那儿，而不是“转到绝对零位”。

---

## C11 / 链接脚本里的四个“栈”

### [T-155] _Min_Stack_Size 管的是 MSP（含所有 ISR），不是线程栈

这里有四个名字，是四回事：

| 名字 | 本工程的值 | 管谁 |
| --- | --- | --- |
| _Min_Stack_Size | 0x400 = 1024 B | MSP：main() 早期阶段以及所有 ISR |
| _Min_Heap_Size | 0x200 = 512 B | 堆（malloc / printf）|
| 三个线程栈 | 1024 ＋ 1536 ＋ 768 = 3328 B | 线程各自的栈（在 .bss 里）|
| TX_BYTE_POOL | CubeMX 给的 1024 B | ThreadX 字节池，本工程不用（app_threadx.c 里有 (void)byte_pool;）|

判断依据是，在 Cortex-M 上异常走 MSP，不走线程的 PSP。stackdepth.ps1 的原话是：“ISRs. On Cortex-M an exception uses MSP, not the thread's PSP, so HAL_xxx_IRQHandler frames do NOT come out of a thread stack.”

所以 _Min_Stack_Size 必须覆盖最深的 ISR 调用链。本工程最深的那条是：

```
CAN1_RX0_IRQHandler 到 HAL_CAN_IRQHandler 到 HAL_CAN_RxFifo0MsgPendingCallback
    再到 s_rxCallback 到 OnCanRx 到 updateFeedback 到 UpdateSensorData    （浮点 + 若干局部）
```

这也是 [T-149] 的第二条理由：回调里不能调 tx_，也因为它花的是 MSP 那 1024 B。所以这条链越短越好，浮点几条指令可以接受，而 printf 绝对不行，带浮点的 printf 会吃掉几百字节栈。

要提醒的是，stackdepth.ps1 不覆盖 ISR，脚本自己也声明了这一点，所以这 1024 B 只能靠人估。

### [T-156] C11 的配置值落在两个地方，必须一致

```
.ioc      : ProjectManager.HeapSize=0x200      ProjectManager.StackSize=0x400
链接脚本   : _Min_Heap_Size = 0x200            _Min_Stack_Size = 0x400
```

C11 的这两个配置值落在两个地方，必须保持一致。其中 .ioc 是给 CubeMX 的输入，链接脚本才是真正生效的地方。

所以要改 C11，就改 .ioc 里的这两个键，CubeMX 会把它们写进链接脚本。

不过要说明的是，这三个文件的行为和 tick 那两个不一样：.ioc 由 GUI 管理，链接脚本 STM32F407XX_FLASH.ld 只在首次生成、之后保留，board/CMakeLists.txt 也是同样的性质，它们不像 [T-109] 里那两个每次都被覆盖。

---












































> 这份文件记的是这个作业**实际怎么走过来的**：配置、实现、踩到的坑、修法，以及**还没做的事**。
> 每条尽量给到可复现的证据（文件、命令、寄存器读数、提交）。
> 为什么把失败也写进来：成功的记录只证明"现在是对的"，**失败 + 根因**才是以后遇到同类现象时能检索的东西。

---

## 0. 协作方式：两条铁律

代码大部分由 agent 写、硬件事实由 agent 查，但**开工方式是我定的**：

| 铁律 | 做法 |
| --- | --- |
| **范围锁死** | 开工前先给"允许改动的文件白名单"，白名单外一个字符都不动 |
| **先对齐后动手** | 先复述要求、列 checklist，确认后再改；一次只做一条 |

定这两条的起因是以前和 AI 协作吃过"完成度过高"的亏：顺手做全局优化、提交物 AI 味重、到后期自己看不出哪一段该维护。

作业原文明确要求写"如何和 agent 合作"，实际分工是：

| 环节 | 谁做的 |
| --- | --- |
| 提要求、定规矩（两条铁律、工作/学习模式协议） | 我 |
| 硬件事实（引脚、晶振、量程） | agent 查原理图/数据手册，我核 |
| 代码实现 | agent，范围限定在清单条目内 |
| 静态自查工具（单元测试 / 可达性 / 栈深） | agent |
| `docs/*.md`、上板验证、建仓与提交 | 我 |

一条后来才总结出来的经验：

> **给 agent 的约束要"可检查"，而不是"要小心"。**
> "不要过度优化"是无效约束；"开工前给白名单"是可检查的——因为白名单本身能被对照。
> 同理，"注意别漏掉调用方"是无效约束，"跑一次 `nm` 看符号在不在 ELF 里"是可检查的。




硬件事实（引脚、晶振、量程、减速比）一律查原理图或数据手册，其中两条值得记：

**① 时钟树是算出来的，不是抄来的。**

C 板晶振 **12.0 MHz**（原理图 X1，PH0/PH1），不是常见的 8 MHz：

```
12 MHz ÷ M6 = 2 MHz → × N168 = 336 MHz → ÷ P2 = 168 MHz  (SYSCLK)
AHB ÷1 = 168 MHz   APB1 ÷4 = 42 MHz（定时器 ×2 = 84）   APB2 ÷2 = 84 MHz（定时器 ×2 = 168）
```

反证来自前序工程的 9 个 `.ioc`：它们对 HSE 有**三种**取值，其中 3 个内部不自洽。**`PLLM=8` 配 12 MHz 只有 126 MHz（慢 25%），而且不报错、串口照常工作**——这是本项目遇到的第一个静默错误。

## 3. 代码实现完了，而且"看起来都对"

四个目录共 28 个业务文件（含头文件），其中我改动/新增 18 个（13 改 + 5 新增）；清单里的 B1–B7、L1–L2、D1–D22、T1–T4 全部实现。

**验证手段（不接板子）**：

| 手段 | 结果 |
| --- | --- |
| 主机端单元测试（HAL 桩 + 假器件） | **173 / 0**（pid 20 + bmi088 56 + motor 42 + m2006 27 + led 28） |
| 评审模拟：只取"会被提交的文件"，干净目录从零 configure + build | 通过；14 个用户代码目标文件全部生成 |
| TODO 扫描 | `bsp` / `libs` / `threads` 各 0 处；`modules` 只剩 1 处框架自带、作业未要求的 `@todo` |

**修掉一个本地看不到、Linux 上必炸的问题**：git 索引里链接脚本是 `STM32F407XX_FLASH.ld`（全大写），磁盘与 CMake 引用是 `STM32F407xx_FLASH.ld`（混合大小写），而 Windows 的 `core.ignorecase=true` 让 git 看不见这个差异。在大小写敏感的文件系统上 clone 会得到大写文件名 ⇒ `cannot open linker script file`，**构建直接失败**。用中间名 `git mv` 绕开（`XX` → 临时名 → `xx`）。

**主机测试抓到一个真 bug**：`libs/src/math.cpp` 的 `LimitABS` 少了一个负号——`if (input < maxvalue) return -maxvalue;`，于是**正的 5.2 被夹成 −1000**，PID 断言从 5/15 变成 20/0。

> **限幅写错比不装更危险**：不装至少方向是对的，写错是整体反号 ⇒ 电机反向满转。
> 而这次**编译完全通过，只有断言抓得到**。

但就在"测试全绿 + 干净构建通过"的状态下，还漏了一个致命缺口（见第 5 节）。

---

## 4. 静默错误（一）：tick 会被重新生成打回 100 Hz

**发现**：想验证"重新生成能否得到同样结果"，于是查 `board/.mxproject`，发现 `Core/Inc/tx_user.h` 和 `Core/Src/tx_initialize_low_level.s` 被登记为 CubeMX 管理，而**这两个文件通篇没有一处 `USER CODE` 块**（对照 `app_threadx.c` 有 22 处）。再与固件包模板对比：`tx_user.h` 里 tick 那行在包里是**被注释掉的 100**，`.s` 里是 `SYSTEM_CLOCK / 100` ⇒ **产品里的 1000 是手工改的**。

**后果**：任何人执行一次 `project generate`，tick **静默退回 100 Hz**——所有 `tx_thread_sleep` 差 10 倍、控制周期 1 ms → 10 ms、标定 4 s → 40 s。而**编译、主机测试、`git status` 全都没有信号**，因为两个值会一起被改回 100，仍然"自洽"。`.ioc` 里没有 tick 参数，放不进去。

**我曾想出的"根治方案"被自己否掉了。** 思路是把 tick 宏搬到能扛过生成的 `board/CMakeLists.txt`（`target_compile_definitions`），让 `.s` 从宏派生。在临时副本里实测：机械上是成功的（宏能传到汇编器；SysTick 重载字面量仍是 `3f 90 02 00` = 167999；移掉定义会响亮失败）。**但放回"重新生成"的场景里推一遍，结论反转**：原状下两个值在同一个"会被覆盖"的集合里，一起回退、保持自洽；把其中一个搬到 CMake 就**解耦**了——重新生成时只回退一个，C 侧宏 1000 / 硬件实际 100，**反而制造出新的不一致**。

**正确对策（零代码改动）**：

```
git commit（保证工作区干净）
→ 重新生成（GUI，见 modes/CubeMX.md）
→ git diff -- board/   ⇒ 这两个文件的变化立刻显形
→ 按需把 tick 改回 1000（两个文件都要改）
→ 必须反汇编核对 SysTick 重载字面量 = 167999
→ 重跑主机测试
```

另外加了两道保险：`threads.hpp` 里的 `static_assert(TX_TIMER_TICKS_PER_SECOND >= 1000U, ...)`（**实测被打回 100 时确实编译失败**），以及 `测试/cubemx-tick.ps1`（一次改 1+3 处，不会漏掉 `.s` 里 `EQU` 那一处）。

**教训**：面对"工具会覆盖我的修改"——**"把值搬到工具管不到的地方"看起来更干净，实际是拆散了必须保持一致的两个值**；**让覆盖这件事变可见**才是零风险的。

---

## 5. 静默错误（二）："写了，但没人调用"（四次）

主机测试一直在验证"算得对不对"，从没问过"**有人会调用它吗**"。这个维度在本项目里漏了四次：

| 次 | 漏的是 | 谁抓住的 | 判据性质 |
| --- | --- | --- | --- |
| 1 | `bsp_Init()` | `nm` | 非虚函数 ⇒ 符号在不在 ELF 里 |
| 2 | `VerifyAccChipID()` / `VerifyGyroChipID()` | **真机红灯常亮** | 虚函数 ⇒ `nm` 失效 |
| 3 | 调用方没 `#include` 头文件 | 编译器 | 声明可见性 |
| 4 | `CreateControlThread()` 忘了取消注释 | `nm` | 同第 1 次 |

**第 1 次的完整经过**：写定时器知识单元时顺带核对"谁启动了 PWM"，全仓库搜 `bsp_Init` → 只有声明和定义；`main.c` 的 `USER CODE 2` 段是空的。用 git 历史排除是我的错：模板首个提交的 `main.c` 里 `USER CODE 2` 也是空的，`git log -S'bsp_Init'` 只显示框架引入过它、**没有任何提交调用过** ⇒ 这是框架自带的接线缺口。

后果用提交前存档副本做负对照，逐符号对比：`bsp_Init`、`HAL_CAN_Start`、`HAL_CAN_ActivateNotification`、`HAL_TIM_PWM_Start` **修复前都不在 ELF 里**——`--gc-sections` 把它们整个丢掉了，因为没有引用者。修 2 行后 **FLASH 40356 → 41912 B**（+1556 B 全是真实代码）。

> **为什么 173 项断言全通过也抓不到**：**单元测试直接调用被测函数，天然假设调用方存在。**
> 它测得出"算得对不对"，**测不出"有没有路径能到它"**。

于是把可达性变成常规检查：`测试/reachability.ps1`（判据是"在 `.obj` 里、不在 `.elf` 里" ⇒ 没有任何路径能到它）。做这个工具时它自己就踩了三个坑，全靠"负对照"发现：`nm` 不加 `-C` 再叠一个 `^_` 过滤会把所有 C++ 方法排除（"已编译函数"从 223 掉到 136，看不出异常）；通配 `*.obj` 会捡到上次配置遗留的陈旧目标文件；第一版脚本还混进了非 ASCII 字节。

**然后这个工具给出的结论（"没有第二处缺口"）又被推翻了**——见 6.1：虚函数会被 vtable 引用，符号永远在 ELF 里。

**后来凝成一条判据——可达性要问两遍**：

1. **符号在不在 ELF 里**（抓非虚函数的死代码）；
2. **有没有真实调用点**（虚函数只能靠全仓库搜调用点，工具给不出这个答案）。

---

## 6. 真机调试：两个只有上板才能暴露的缺口

环境：一块 C 板 + ST-Link（SWD）+ Cortex-Debug，仅由 ST-Link 的 3.3 V 供电。

### 6.1 红灯常亮：ID 校验函数从未被调用

- **现象**：上电后红灯闪约 4 秒（标定中）→ **红灯常亮**。按设计，红常亮 = 初始化失败。
- **定位链**：红常亮 → 初始化失败 → 查 `self_test.INIT_ERR` → 它**默认就是 `true`**，而全项目**只有置 `true` 的地方、没有置 `false` 的地方** → 再查"谁该把它清零" → `VerifyAccChipID()` / `VerifyGyroChipID()` **全仓库没有任何调用点**（只在注释和 `docs/spi.md` 的文字里出现过）。
- **关键判断**：**红灯常亮不能说明 BMI088 坏**——一块完好的板子也必然如此（`ReportImuInit(!INIT_ERR && …)` 恒为 false）。所以"照灯排查"之前必须先确认"灯本身可信"。
- **修复 4 行**：`threads/src/imuthread.cpp` 在 `Config()` 之后补 2 行调用；`modules/src/BMI088.cpp` 两个 ID 校验的**成功分支**各补 `INIT_ERR = false;`。
- **实测**：红灯闪约 4.4 秒后熄灭，绿/蓝开始翻转 ⇒ 一次性证明**加速度计 ID = `0x1E`、陀螺仪 ID = `0x0F` 都读到了**（SPI1 + 软件片选 + Mode 3 + 5.25 MHz 全对），且标定通过。

### 6.2 加速度计恒为 0：寄存器写对了 ≠ 芯片进入了那个状态

- **现象**：红灯灭之后 `acc` 恒为 `[0,0,0]`，而**同一颗芯片**的 ID 读到了 `0x1E`、温度读到了 `22.4 °C`、陀螺仪三轴正常。
- **排除过程**（全程用 SWD 直接读芯片寄存器）：读回 `PWR_CTRL(0x7D)=0x04`、`PWR_CONF(0x7C)=0x00`、`ACC_CONF(0x40)=0xAB`——**三个配置寄存器都真的写进去了，但仍然没有数据**。所以问题不是"没写进去"，而是**芯片没有真正进入测量状态**。
- **根因（有官方一手出处）**：Bosch `BMI08x_SensorAPI` 的 `bmi08a_set_power_mode()` 顺序是 `PWR_CONF=0x00` → 5 ms → `PWR_CTRL=0x04` → 5 ms，**而工程写反了**：enable 位是在芯片仍 suspend 时写下的，寄存器存下了值，芯片却不开始测量。
- 官方还要求两件工程里缺的事：**软复位后补一次 dummy SPI 读**；写完 `ACC_CONF` 后延时 **40 ms**（工程只有 5 ms）。
- 顺手核实了 `ACC_CONF = 0xAB` 是对的：`BMI08_ACCEL_BW_NORMAL = 0x0A`、`BMI08_ACCEL_ODR_800_HZ = 0x0B`，`0x0A<<4 | 0x0B = 0xAB`。
- **修复后实测**：`ReadAccData → {x=-0.074, y=0.208, z=9.724}`（静止时测到重力）；`self_test` 六项错误标志全部 `false`。

### 6.3 真机顺带拿到的读数

| 项 | 读数 | 对照 |
| --- | --- | --- |
| 时钟树 | `RCC_CR=0x03037383`、`PLLCFGR=0x04402A06`（PLLM=6/PLLN=168/PLLQ=4）、`CFGR=0x940A`、`SystemCoreClock=168000000` | 12 MHz + PLLM=6 的推导 |
| tick | SysTick `LOAD=167999`；`_tx_timer_system_clock` 10 秒 ≈ 10000 | 1 kHz 属实，第 4 节的隐患排除 |
| 加热 | TIM10 `ARR=49 / PSC=167` ⇒ 20 kHz；`CEN=1`；`CCR1` 随温度变化（22 °C 时饱和到 100%） | `PWM_Init` 启动 `htim10` 生效 |
| CAN | `MCR=0x10010`（NART=1、ABOM=0）、`BTR=0x290002`（1 Mbit/s）、`MSR.INIT` 由 1→0 | `bsp_Init()` 修复生效 |
| LED | `GPIOH.ODR`：初始化中仅红灯、成功后红灭绿蓝闪 | 灯效三状态机 |

**上板过程里踩的工具坑**：命令行烧录用 `Configuration\F4\stlink.cfg` 会报 `Debug adapter doesn't support 'hla_swd' transport`（老式 HLA 写法），改用 `-f interface/stlink.cfg -f target/stm32f4x.cfg`。

### 6.4 真机上还没做的

- **电机**：开环（手转轮子看 `speedFdb` / `positionFdb` 的符号与回绕）和闭环（`SPD_MODE` / `POS_MODE`）**都还没在真机上验证过**，目前只有主机端断言。
- **串口**：`bsp_usart` 实现了收发，但 `USART_Transmit` / `USART_Receive` **全项目没有调用 ⇒ 串口目前没有任何输出**，硬件调试时只有三色灯一个信息通道。
- **故障注入**：拔 CAN 线看蓝灯停、拔 SPI 排线看绿灯停——没做，所以"监控有效"还没有反向证据。
- 因此验收里的 V1（各外设单独验证）与 V3（电机）**在真机上仍是部分完成**。

---

## 7. 不接板子也能量出来的东西

### 7.1 栈够不够

`threads.hpp` 里 1024 / 1536 / 768 一直是凭判断选的。用 `.su`（`-fstack-usage`）给出的帧大小 + `objdump` 的 `bl` 调用图算最深路径：

```
thread    stack   worst-case (static)   margin
control    1024          248             1.3x
imu        1536          368             1.3x
alive       768          328             1.3x
```

**更有价值的不是总数，是路径的形状**：三条最深路径**都经过 `_tx_thread_system_suspend`（128 B）** ⇒ 只要线程会 sleep 或等信号量，就有约 **300 B 的"地板"**。所以业务最轻的 `alive` 反而是三个里最紧的（328/768）。

限制也要写清楚：它不跟踪间接调用（函数指针），**故意不含 ISR**（异常用 MSP、线程用 PSP），所以它是**下界不是上界**，不能替代上板的高水位实测。

### 7.2 中断优先级 × RTOS：为什么"全是 0"是安全的

CubeMX 把所有使能中断都设成 preemption priority **0**（最高），看起来危险。判据是两步：

1. **先看内核用哪种屏蔽**：全仓库没有 `#define TX_PORT_USE_BASEPRI`；反汇编整个 `.text`，`msr basepri` **0 次**、`cpsid i` **29 次** ⇒ ThreadX 走 `PRIMASK` 分支 ⇒ **内核临界区屏蔽所有可屏蔽中断，任何 ISR 都插不进来**。
2. **再看 ISR 有没有调 API**：**没有任何 ISR 调用 `tx_*`**（唯一的应用 ISR 是 `HAL_CAN_RxFifo0MsgPendingCallback`，只做解析）。

结论：优先级 0 是安全的，**而安全的原因不在中断里，在内核的编译配置里**。代价是中断延迟更大（本项目临界区短、中断不密集，可以接受）。只做第一步或只做第二步，都会得出错误结论。

两个记录项（不是 bug）：所有中断同级 ⇒ 互相不能抢占；CAN ISR 写反馈、control 线程读反馈之间存在**没有写进代码的并发说明**（单个 32 位浮点写是原子的，`positionFdb` 与 `speedFdb` 可能来自相邻两帧，对电机环实际无害）。

### 7.3 工具本身的问题：会"静默排除"的过滤器

做上面这些检查时，同一类错误反复出现——**工具不报错，只是悄悄少看了东西**。累计遇到九次，形态不同、后果一致：

- `nm` 不加 `-C` 再叠一个 `^_` 过滤 ⇒ **所有 C++ 函数隐形**（"已编译函数"从 223 掉到 136，看起来正常）；
- 通配 `*.obj` ⇒ 捡到上一次配置留下的陈旧目标文件；
- 按目录前缀过滤 `.su` ⇒ 文件名只有 basename ⇒ **81 个业务函数只列出了 4 个**；
- 回调正则要求 `HAL_xxx_` 后紧跟 `Callback` ⇒ 漏掉 `HAL_CAN_RxFifo0MsgPendingCallback`（全场最要紧的那个 ISR）；
- 拿 `41912` 去核对比文件里写的 `41 912 B` ⇒ 误报"缺"；
- 把 `$t.Length` 当成字节数（其实是字符数）⇒ 结论反了；
- 引用正则多带了一个 `}` ⇒ 误报"没写"；
- 按 URL 精确匹配比对资料登记 ⇒ 同一文档的 `/resource/` 与 `/resource/en/` 被当成两个；
- 把"索引没提到"和"单元没引用"混成一件事。

**唯一可靠的防线是两条：① 负对照（故意塞一个"必须出现"的样本，看工具认不认得）；② 反查（拿到"缺/少"的结论时，去看它到底拿什么在比）。** 这两条对"我用 grep 找了一遍没找到"这种手工排查同样成立。

---

## 8. 提交之后，用"陌生人视角"重来一遍

`git push` 成功不等于评审人能看到。所以做了四步：

1. **匿名可达性**：禁用凭据助手与终端提示后 `git ls-remote --heads origin` 仍成功 ⇒ 仓库确实是 public。
2. **匿名克隆 vs 本地**：HEAD / tree / 提交数 / 文件数逐字一致；`board/build/` 不在克隆里 ⇒ `.gitignore` 生效，中间产物没有混进提交物。
3. **干净克隆从零构建**：`cmake --preset Debug` + build 通过，FLASH / RAM 与本地完全相同（`41912 B` / `10048 B`），**493 个符号一致**；第 5 节修掉的 `HAL_CAN_Start`、`HAL_TIM_PWM_Start` 确实在推送的产物里。
4. **两个 ELF 的哈希不同，原因被定量定位**：逐段比对 `.text` / `.rodata` / `.data` **逐字节相同**；在整个 ELF 里搜构建路径字符串，`reviewer-clone` 本地 0 次 / 克隆 417 次——因为编译开了 `-g3`，**源文件的绝对路径被写进了调试信息**。换个目录构建，路径就变，哈希当然变，而机器码一模一样。

**关于仓库里那次大规模的 vendor 文件删除**：重新生成后 `git add -A` 里有 **1350 个删除**（1071 个 DSP / NN / Cortex-A 部件 + 一整套重复的 HAL），这是 CubeMX 重新生成后的正常结果，**不是误删**——业务目录 0 删除。

---

## 9. 还没做完的

（写成"没做"，不写"基本完成"——文档里的肯定句会让所有人、包括未来的我停止检查。）

| # | 项 | 现状 |
| --- | --- | --- |
| 1 | 电机真机验证（开环符号/回绕、闭环 SPD / POS） | 未做，只有主机端断言 |
| 2 | 串口输出（`USART_Transmit` 无人调用） | 未做 ⇒ 目前只有三色灯一个信息通道 |
| 3 | 故障注入验监控（拔 CAN / 拔 SPI） | 未做 |
| 4 | `bsp/` 是否需要 SPI / I²C 封装 | 待向课程确认（现按"IMU 可直接调 HAL"的读法实现） |
| 5 | `reachability.ps1` 增加虚函数调用点检查 | 待做 |
| 6 | `TEMP_CTRL_ERR` 默认 `true` 且全项目无人写 | 目前无人读它，无害，但要知道 |
| 7 | 文档：`ahrs.md`（选做）与其余几份的补充 | 进行中 |

---

## 10. 流程本身没执行到位的地方

工作模式里写了两个"必须停下等人"的卡点：**改动前给白名单等确认**、**出检测题等作答**。实际执行是：

- **复述环节从未发生**，**题库约 60 道题一道都没被作答过**；
- 后期有四处改动是**直接改的**（虽然每次都说明了对应清单哪一条，但没有先给白名单等确认）；
- "开工前拉最新要求"只在开头做过一次，中间断了 24 轮。

结果是：**代码是可以交付的，但"我能否自主维护它"这件事至今没有证据**——而这恰恰是这套工作流存在的唯一目的。补救办法是补上这两步：对改动最大、最容易错的几条做抽查式复述，并实际作答一批检测题。

---

## 11. 这次总结出来的几条

1. **测试通过 ≠ 系统能工作。** 173 项断言 + 干净构建全通过，仍然漏了 `bsp_Init`；原因是**单元测试天然假设调用方存在**。
2. **静默错误要用"会响的检查"来抓**：`static_assert`（tick）、`nm`（可达性）、`.su` + 调用图（栈）、反汇编（字面量）、`git diff`（生成覆盖）。这些坑的共同点是**编译、测试、`git status` 全都没有信号**。
3. **可达性要问两遍**：① 符号在不在 ELF 里；② 有没有真实调用点（虚函数只能搜）。
4. **"我找不到"≠"它不存在"。** 两条否定性结论后来都被推翻，靠的是换方法 + 阳性对照。
5. **给约束要"可检查"。** "不要过度优化"无效，"开工前给白名单"有效；"注意别漏调用方"无效，"跑一次 `nm`"有效。
6. **工具会静默排除**，防线是负对照 + 反查。
7. **文档里每个"已完成"都要回实际状态点一遍。** 本项目连续三次审计各抓到一处"文档写已补全、代码里还是占位"。写成肯定句，就等于让所有人停止检查——比代码 bug 危险。

---

## 附：配置这一层我是怎么学会的 —— 从零重建练习工程

提交物基本是一次成型的（写完就跑测试）。为了搞清"每个文件为什么存在"，我又用同一套配置从零重建了一个练习工程（独立仓库）：17 个提交、四层共 28 个手写文件、C1–C13 全部自己在 CubeMX 里点。

**节奏**：每个阶段都是"**配置 → 封装 → 验证 → 提交**"——C3 配完就有 `bsp_pwm`，C4 配完就有 `bsp_usart`，C5 配完就有 `bsp_can`。
**而一个例外本身也是认知**：C6（SPI）、C7（I²C）、C8（加热）**没有对应的 bsp 层**，直接由 `modules` 调用 HAL——和提交物一致（原文那句"IMU 模块可直接调用 HAL SPI 和 HAL GPIO"）。

几个印象最深的坑：

| 现象 | 根因 | 判据 |
| --- | --- | --- |
| `multiple definition of 'PWM_Init()'` ×5 | `bsp.cpp` 里写成了 `#include "bsp_pwm.cpp"` | **两条报错一条指向 `.obj`、一条指向源码行 ⇒ 真的被编译了两次**（我一开始以为是在报缓存的旧错） |
| `PWN_Stop` / `uint8_t Size` 签名不匹配 | 声明与定义不一致 | **"声明了但没定义"不报错，只有调用时才炸** |
| `HAL_UARTEx_ReceiveTOIdle_DMA` 拼错 + 约 10 条 IntelliSense 红 | 全是噪音 | **IntelliSense 的红是候选，编译器的红才是判决** |
| CAN 波特率算出来 1166666 bit/s | `BS2=1` | **`Prescaler × (1 + BS1 + BS2) = APB1 / 波特率 = 42`**；改成 `BS2=3` 后是 1 Mbps |

配置层面的两个可复现结论：

- **PWM 的频率公式只有 ARR 在变**：`PSC = 168-1` ⇒ 计数频率 1 MHz。TIM1 `ARR = 20000-1` ⇒ 20 ms = 50 Hz，**计数单位正好是 1 µs，所以 CCR 直接就是脉宽**（Pulse=1500 ⇒ 1.5 ms 中位）；TIM10 `ARR = 50-1` ⇒ 20 kHz 加热。**`PWM_Init()` 里必须启动 `htim10`**，漏了就是加热不出力。
- **引脚是"先到先得"的**：成品里那些看起来奇怪的引脚（SPI1 用 PB3/PB4/PA7 而不是 PA5/6/7、USART1 RX 用 PB7 而不是 PA10）都是避开外设冲突后的选择。查引脚要分两种"手册"：原理图（线接到哪）和数据手册 Table 9（这个脚能当什么）。

第一次主机测试就 `led` 28 / 0 通过 —— 说明"按接口写、不抄实现"这条路可行；但同一件事也说明**主机测试只能验证"算得对不对"**——这一点在后面反复撞上。

练习工程自己还剩的缺口（如实）：

- `HAL_UARTEx_RxEventCallback` **只有注释、没有实现** ⇒ 接收机制武装了，但数据到了没人处理。**这不是编译错误，是功能缺口**——和 `bsp_Init` 那次同类："配了 / 写了"和"会跑 / 有人接"是两件事。
- `bsp_usart.cpp` 的 `LastWriteTime` **比 `guoqing.elf` 新** ⇒ **当前的构建产物不是最新源码编出来的**（那次改的是注释，所以符号没错，但结论要按实情写）。
- C1–C13 都只到"配齐了"，**没有上板验证过**。
