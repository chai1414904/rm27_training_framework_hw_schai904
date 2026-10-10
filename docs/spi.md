# SPI

SPI = Serial Peripheral Interface。

## 全双工

**全双工**：有两条独立的数据线，可以在同一时刻双向传输。

**例：SPI 有四根线**，其中**两根数据线（MOSI、MISO）**。
在**同一个 SCK 沿**，双方同时发送、接收**一位**。

## 片选（CS）：SPI 靠它"点名"

SPI **没有地址**，选设备靠**拉低它的 CS**（硬件选）。

- SCK / MOSI / MISO 是**所有从机共用**的；**只有 CS 是每个从机各一根**
- **CS 拉低 = 开始；CS 拉高 = 结束** —— 它不只是"点名"，还是"一句话的边界"
- 从机靠**数 SCK 沿**知道够不够 8 位，靠 **CS** 知道这整段是**一次事务**
- **没被选中的从机要放开（高阻）自己的 MISO** —— 否则两个从机同时驱动 MISO 就会打架
- 忘了拉高 CS ⇒ **两笔事务被拼成一笔**：一个从机听错，两个从机打架

> **再加一个 SPI 器件，只需要多一根线：它自己的 CS。**

## 没有 ACK —— 确认靠"回读"

**SPI 协议里没有应答**（这点和 I²C 不同，I²C 每个字节后都有一位 ACK）。

- 协议层：**没有**
- 应用层怎么补：**回读** —— 读芯片 ID、回读寄存器、读状态位
- 本工程就是这么做：`VerifyAccChipID()` 读加速度计的芯片 ID（期望 `0x1E`），
  对不上就置 `ACC_CHIP_ID_ERR` / `INIT_ERR`，再由 `alive` 线程监控
  （代码：`modules/src/BMI088.cpp`）

## 数据位顺序

**最高位先发（MSB first）**：先发 bit7，最后发 bit0。

## 本工程的 SPI1 配置

| 项 | 值 | 为什么这么选 |
| --- | --- | --- |
| Mode | Full-Duplex **Master** | STM32 出时钟，BMI088 是从机 |
| Direction | 2 Lines（全双工）| 收的时候必须同时发出去 |
| Data Size | 8 Bits | 地址和数据都是字节 |
| CPOL / CPHA | High / 2Edge ⇒ **Mode 3** | 本项目按器件手册配成 Mode 3（CPOL=1、CPHA=1）|
| Hardware NSS | **Disable**（用软件 NSS）| 硬件 NSS 会在每次传输后自动拉高，打断"地址 + 连续数据"的突发 |
| Prescaler | 16 | APB2 = 84 MHz，84 ÷ 16 = **5.25 MHz** |
| First Bit | MSB | 最高位先发 |

**引脚**：SCK = **PB3**，MISO = **PB4**，MOSI = **PA7**
**片选**：加速度计 = **PA4**，陀螺仪 = **PB0**（都在 `gpio.c` 里配成**推挽输出**，上电即置高）

> ⚠️ `PB3 = JTDO`、`PB4 = NJTRST`：调试口必须选 **SWD**（Serial Wire），
> 否则被 JTAG 占用，SPI1 不工作。
> ⚠️ SPI1 的三个脚**跨端口、不连续**（PB3 / PB4 / PA7），不要以为默认是 PA5/PA6/PA7 —— 以原理图为准。
