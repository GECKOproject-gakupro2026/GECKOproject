/**
  ******************************************************************************
  * @file    console.cpp
  * @brief   printf retarget and character input over USART1 (ST-LINK VCP).
  *
  *          RX uses GPDMA1 (linked-list, circular queue of one node - the
  *          only circular mode STM32U5's GPDMA supports) into a ring
  *          buffer. The first implementation used HAL_UART_Receive_IT()
  *          (one byte per interrupt): at 921600 baud, with the 50 Hz
  *          telemetry loop keeping the CPU busy several hundred
  *          microseconds per pass, the re-arm gap between consecutive IT
  *          receptions was wide enough to silently drop bytes - console
  *          commands and, worse, OTA frame bytes went missing under load.
  *          DMA writes to memory autonomously with no CPU involvement per
  *          byte, so reception is immune to how busy the main loop is;
  *          Console_GetChar() just polls how far the circular DMA write
  *          pointer has advanced (CBR1 block counter) and drains newly
  *          arrived bytes from the buffer.
  ******************************************************************************
  */
#include "console.h"
#include "main.h"

#include <cstdio>

extern UART_HandleTypeDef huart1;

/* newlib-nano: pull in the floating-point printf implementation (%f) */
asm(".global _printf_float");

namespace
{
constexpr size_t kRxBufSize = 1024; /* must fit an OTA frame (1024+8 max) with margin */
uint8_t rxDmaBuf[kRxBufSize];
size_t rxTail = 0; /* next byte index the app hasn't consumed yet */

DMA_HandleTypeDef hdmaUsart1Rx = {};
DMA_QListTypeDef rxQueue = {};
DMA_NodeTypeDef rxNode = {};
bool dmaStarted = false;

size_t dmaWriteIndex()
{
  /* CBR1[15:0] (block size remaining) counts down from kRxBufSize to 0
   * over each lap of the circular node; the write position is the
   * complement. */
  DMA_Channel_TypeDef *ch = reinterpret_cast<DMA_Channel_TypeDef *>(hdmaUsart1Rx.Instance);
  uint32_t remaining = ch->CBR1 & 0xFFFFU;
  return (kRxBufSize - remaining) % kRxBufSize;
}

bool startRxDma()
{
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  DMA_NodeConfTypeDef nodeConfig = {};
  nodeConfig.NodeType = DMA_GPDMA_LINEAR_NODE;
  nodeConfig.Init.Request = GPDMA1_REQUEST_USART1_RX;
  nodeConfig.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
  nodeConfig.Init.Direction = DMA_PERIPH_TO_MEMORY;
  nodeConfig.Init.SrcInc = DMA_SINC_FIXED;
  nodeConfig.Init.DestInc = DMA_DINC_INCREMENTED;
  nodeConfig.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_BYTE;
  nodeConfig.Init.DestDataWidth = DMA_DEST_DATAWIDTH_BYTE;
  nodeConfig.Init.Priority = DMA_HIGH_PRIORITY;
  nodeConfig.Init.SrcBurstLength = 1;
  nodeConfig.Init.DestBurstLength = 1;
  nodeConfig.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
  nodeConfig.Init.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  nodeConfig.Init.Mode = DMA_NORMAL;
  nodeConfig.SrcAddress = reinterpret_cast<uint32_t>(&huart1.Instance->RDR);
  nodeConfig.DstAddress = reinterpret_cast<uint32_t>(rxDmaBuf);
  nodeConfig.DataSize = kRxBufSize;
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
  /* TrustZone: USART1 and this buffer are secure (same requirement as the
   * audio GPDMA channels in telemetry.cpp) */
  nodeConfig.SrcSecure = DMA_CHANNEL_SRC_SEC;
  nodeConfig.DestSecure = DMA_CHANNEL_DEST_SEC;
#endif

  if (HAL_DMAEx_List_BuildNode(&nodeConfig, &rxNode) != HAL_OK)
  {
    return false;
  }
  if (HAL_DMAEx_List_InsertNode(&rxQueue, nullptr, &rxNode) != HAL_OK)
  {
    return false;
  }
  if (HAL_DMAEx_List_SetCircularMode(&rxQueue) != HAL_OK)
  {
    return false;
  }

  hdmaUsart1Rx.Instance = GPDMA1_Channel1;
  hdmaUsart1Rx.InitLinkedList.Priority = DMA_HIGH_PRIORITY;
  hdmaUsart1Rx.InitLinkedList.LinkStepMode = DMA_LSM_FULL_EXECUTION;
  hdmaUsart1Rx.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
  hdmaUsart1Rx.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  hdmaUsart1Rx.InitLinkedList.LinkedListMode = DMA_LINKEDLIST_CIRCULAR;

  if (HAL_DMAEx_List_Init(&hdmaUsart1Rx) != HAL_OK)
  {
    return false;
  }
  if (HAL_DMAEx_List_LinkQ(&hdmaUsart1Rx, &rxQueue) != HAL_OK)
  {
    return false;
  }

#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
  /* TrustZone: the CHANNEL itself must be secure + privileged as well -
   * without this the node fetch from secure SRAM fails (ULE) and the
   * channel silently aborts (same lesson as the audio path). */
  if (HAL_DMA_ConfigChannelAttributes(
          &hdmaUsart1Rx, DMA_CHANNEL_SEC | DMA_CHANNEL_PRIV |
                             DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC) != HAL_OK)
  {
    return false;
  }
#endif

  __HAL_LINKDMA(&huart1, hdmarx, hdmaUsart1Rx);

  /* Start the channel directly (bypassing HAL_UART_Receive_DMA, which only
   * knows the plain non-linked-list DMA API) and disable its IRQ - nothing
   * is serviced by interrupt, Console_GetChar() polls CBR1 instead. */
  if (HAL_DMAEx_List_Start(&hdmaUsart1Rx) != HAL_OK)
  {
    return false;
  }
  ATOMIC_SET_BIT(huart1.Instance->CR3, USART_CR3_DMAR);
  huart1.RxState = HAL_UART_STATE_BUSY_RX;

  rxTail = 0;
  return true;
}
} // namespace

extern "C" int __io_putchar(int ch)
{
  /* Telemetry uses interrupt TX on the same UART: wait for it to drain so
   * log characters are not rejected with HAL_BUSY */
  uint32_t t0 = HAL_GetTick();
  while (huart1.gState != HAL_UART_STATE_READY && (HAL_GetTick() - t0) < 20U)
  {
  }
  HAL_UART_Transmit(&huart1, reinterpret_cast<uint8_t *>(&ch), 1, 100);
  return ch;
}

extern "C" int _write(int file, char *ptr, int len)
{
  (void)file;
  for (int i = 0; i < len; i++)
  {
    __io_putchar(ptr[i]);
  }
  return len;
}

extern "C" int Console_GetChar(uint32_t timeout_ms)
{
  if (!dmaStarted)
  {
    dmaStarted = startRxDma();
    printf("[CONSOLE] RX DMA start: %s, CBR1=0x%08lX CCR=0x%08lX\r\n",
           dmaStarted ? "OK" : "FAILED",
           reinterpret_cast<DMA_Channel_TypeDef *>(hdmaUsart1Rx.Instance)->CBR1,
           reinterpret_cast<DMA_Channel_TypeDef *>(hdmaUsart1Rx.Instance)->CCR);
    if (!dmaStarted)
    {
      return -1;
    }
  }

  uint32_t t0 = HAL_GetTick();
  do
  {
    size_t head = dmaWriteIndex();
    if (head != rxTail)
    {
      uint8_t ch = rxDmaBuf[rxTail];
      rxTail = (rxTail + 1U) % kRxBufSize;
      return ch;
    }
  } while (timeout_ms != 0U && (HAL_GetTick() - t0) < timeout_ms);
  return -1;
}
