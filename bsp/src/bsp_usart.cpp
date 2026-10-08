#include "bsp_usart.hpp"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart6;

/// 阻塞发送的超时时间：留出余量即可，不能用 HAL_MAX_DELAY，否则串口异常时会永久卡死
#define USART_BLOCK_TIMEOUT_MS 100U

void USART_Init(void)
{
  USART1_Init();
  USART6_Init();
}

void USART6_Init()
{
  // 清掉上电过程中可能残留的空闲标志与溢出标志，避免刚使能中断就误触发一次
  __HAL_UART_CLEAR_IDLEFLAG(&huart6);
  __HAL_UART_CLEAR_OREFLAG(&huart6);

  // DMA 通道与引脚复用由 CubeMX 生成的 HAL_UART_MspInit 配好；
  // 空闲中断无需在此手动打开：HAL_UARTEx_ReceiveToIdle_DMA 会自行使能 IDLEIE。
  // 接收由 USART_Receive() 启动，缓冲区由调用方提供。
};

void USART1_Init()
{
  __HAL_UART_CLEAR_IDLEFLAG(&huart1);
  __HAL_UART_CLEAR_OREFLAG(&huart1);
};

void USART_Transmit(UART_HandleTypeDef *huart, uint8_t *pData, uint16_t Size, enum USART_Mode mode)
{
  if (huart == nullptr || pData == nullptr || Size == 0U)
  {
    return;
  }

  switch (mode)
  {
  case USART_MODE_BLOCK:
    (void)HAL_UART_Transmit(huart, pData, Size, USART_BLOCK_TIMEOUT_MS);
    break;
  case USART_MODE_DMA:
    (void)HAL_UART_Transmit_DMA(huart, pData, Size);
    break;
  case USART_MODE_IT:
    (void)HAL_UART_Transmit_IT(huart, pData, Size);
    break;
  default:
    break;
  }
}

void USART_Receive(UART_HandleTypeDef *huart, uint8_t *pData, uint16_t Size)
{
  if (huart == nullptr || pData == nullptr || Size == 0U)
  {
    return;
  }

  // 收到 IDLE（一帧结束）或缓冲区填满时，HAL 调用 HAL_UARTEx_RxEventCallback(huart, Size)，
  // 长度由 HAL 按 DMA 剩余计数算出；本次接收到此结束，调用方处理完需再次调用本函数重新武装。
  (void)HAL_UARTEx_ReceiveToIdle_DMA(huart, pData, Size);
}
