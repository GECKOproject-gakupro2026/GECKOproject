/**
  ******************************************************************************
  * @file    tests_memory.cpp
  * @brief   Memory tests: external OSPI NOR/PSRAM, internal flash NV data,
  *          SRAM ring buffer (volatile data management).
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "b_u585i_iot02a_ospi.h"

#include <cstdio>
#include <cstring>

/* CubeMX-generated OCTOSPI1 handle (main.c) */
extern "C" OSPI_HandleTypeDef hospi1;

namespace apptest
{

namespace
{
void fillPattern(uint8_t *buf, size_t len, uint8_t seed)
{
  for (size_t i = 0; i < len; i++)
  {
    buf[i] = static_cast<uint8_t>(seed ^ (i * 13U));
  }
}
} // namespace

Result testOspiNor()
{
  BSP_OSPI_NOR_Init_t init = {};
  init.InterfaceMode = BSP_OSPI_NOR_OPI_MODE;
  init.TransferRate = BSP_OSPI_NOR_STR_TRANSFER;
  if (BSP_OSPI_NOR_Init(0, &init) != BSP_ERROR_NONE)
  {
    printf("  NOR init failed\r\n");
    return Result::Fail;
  }

  uint8_t id[3] = {};
  if (BSP_OSPI_NOR_ReadID(0, id) != BSP_ERROR_NONE)
  {
    printf("  NOR ReadID failed\r\n");
    return Result::Fail;
  }
  printf("  NOR JEDEC ID = %02X %02X %02X (expect C2 85 3A)\r\n", id[0], id[1], id[2]);
  if (id[0] != 0xC2U)
  {
    return Result::Fail;
  }

  /* Erase last 64KB block, write a pattern, read back */
  uint8_t wbuf[256];
  uint8_t rbuf[256] = {};
  fillPattern(wbuf, sizeof(wbuf), 0x5AU);
  if (BSP_OSPI_NOR_Erase_Block(0, CFG_NOR_TEST_ADDR, BSP_OSPI_NOR_ERASE_64K) != BSP_ERROR_NONE)
  {
    printf("  NOR erase failed\r\n");
    return Result::Fail;
  }
  while (BSP_OSPI_NOR_GetStatus(0) == BSP_ERROR_BUSY)
  {
    HAL_Delay(10);
  }
  if (BSP_OSPI_NOR_Write(0, wbuf, CFG_NOR_TEST_ADDR, sizeof(wbuf)) != BSP_ERROR_NONE ||
      BSP_OSPI_NOR_Read(0, rbuf, CFG_NOR_TEST_ADDR, sizeof(rbuf)) != BSP_ERROR_NONE)
  {
    printf("  NOR write/read failed\r\n");
    return Result::Fail;
  }
  if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0)
  {
    printf("  NOR verify mismatch\r\n");
    return Result::Fail;
  }
  printf("  NOR erase/write/read 256B at 0x%08lX OK\r\n", (uint32_t)CFG_NOR_TEST_ADDR);
  return Result::Pass;
}

Result testOspiPsram()
{
  /* Release the CubeMX-initialized OCTOSPI1 so the BSP config applies cleanly */
  static bool mxOspiReleased = false;
  if (!mxOspiReleased)
  {
    HAL_OSPI_DeInit(&hospi1);
    mxOspiReleased = true;
  }

  if (BSP_OSPI_RAM_Init(0) != BSP_ERROR_NONE)
  {
    printf("  PSRAM init failed\r\n");
    return Result::Fail;
  }

  /* The BSP enables the OSPI delay block only in memory-mapped mode, but
   * indirect reads sample through it as well - configure it here (same
   * empiric PhaseSel/4 as the BSP does). */
  HAL_OSPI_DLYB_CfgTypeDef dlyb = {};
  if (HAL_OSPI_DLYB_GetClockPeriod(&hospi_ram[0], &dlyb) == HAL_OK)
  {
    dlyb.PhaseSel /= 4U;
    (void)HAL_OSPI_DLYB_SetConfig(&hospi_ram[0], &dlyb);
  }

  uint8_t id[2] = {};
  if (BSP_OSPI_RAM_ReadID(0, id) == BSP_ERROR_NONE)
  {
    printf("  PSRAM ID = %02X %02X (expect 0D 5D)\r\n", id[0], id[1]);
  }

  uint8_t wbuf[256];
  uint8_t rbuf[256] = {};
  fillPattern(wbuf, sizeof(wbuf), 0xC3U);
  if (BSP_OSPI_RAM_Write(0, wbuf, 0x0000U, sizeof(wbuf)) != BSP_ERROR_NONE ||
      BSP_OSPI_RAM_Read(0, rbuf, 0x0000U, sizeof(rbuf)) != BSP_ERROR_NONE)
  {
    printf("  PSRAM write/read failed\r\n");
    return Result::Fail;
  }
  if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0)
  {
    printf("  PSRAM verify mismatch: w=[%02X %02X %02X %02X] r=[%02X %02X %02X %02X]\r\n",
           wbuf[0], wbuf[1], wbuf[2], wbuf[3], rbuf[0], rbuf[1], rbuf[2], rbuf[3]);
    return Result::Fail;
  }
  printf("  APS6408 write/read 256B OK\r\n");
  return Result::Pass;
}

Result testInternalFlash()
{
  /* Non-volatile data management demo: erase a reserved secure page and
   * program a quad-word record, as the model/config store will do. */
  alignas(16) static const uint8_t record[16] = {
      'N', 'V', 'T', 'S', 1, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44};

  HAL_FLASH_Unlock();

  FLASH_EraseInitTypeDef erase = {};
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks = CFG_NVTEST_BANK;
  erase.Page = CFG_NVTEST_PAGE;
  erase.NbPages = 1;
  uint32_t pageError = 0;
  if (HAL_FLASHEx_Erase(&erase, &pageError) != HAL_OK)
  {
    HAL_FLASH_Lock();
    printf("  flash erase failed (page error 0x%08lX)\r\n", pageError);
    return Result::Fail;
  }

  if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, CFG_NVTEST_ADDR,
                        reinterpret_cast<uint32_t>(record)) != HAL_OK)
  {
    HAL_FLASH_Lock();
    printf("  flash program failed\r\n");
    return Result::Fail;
  }
  HAL_FLASH_Lock();

  if (memcmp(reinterpret_cast<const void *>(CFG_NVTEST_ADDR), record, sizeof(record)) != 0)
  {
    printf("  flash verify mismatch\r\n");
    return Result::Fail;
  }
  printf("  internal flash page %u erase+program+verify OK (persists across reset)\r\n",
         CFG_NVTEST_PAGE);
  return Result::Pass;
}

/* Fixed-capacity ring buffer as used for sensor data buffering (volatile). */
template <typename T, size_t N>
class RingBuffer
{
public:
  bool push(const T &v)
  {
    if (count_ == N) return false;
    buf_[head_] = v;
    head_ = (head_ + 1) % N;
    count_++;
    return true;
  }
  bool pop(T &v)
  {
    if (count_ == 0) return false;
    v = buf_[tail_];
    tail_ = (tail_ + 1) % N;
    count_--;
    return true;
  }
  size_t size() const { return count_; }

private:
  T buf_[N] = {};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
};

Result testSramBuffer()
{
  static RingBuffer<int16_t, 512> rb;

  /* Fill, overflow check, then drain and verify FIFO order */
  for (int i = 0; i < 512; i++)
  {
    if (!rb.push(static_cast<int16_t>(i - 256)))
    {
      printf("  premature overflow at %d\r\n", i);
      return Result::Fail;
    }
  }
  if (rb.push(0))
  {
    printf("  overflow not detected\r\n");
    return Result::Fail;
  }
  for (int i = 0; i < 512; i++)
  {
    int16_t v = 0;
    if (!rb.pop(v) || v != static_cast<int16_t>(i - 256))
    {
      printf("  FIFO order broken at %d\r\n", i);
      return Result::Fail;
    }
  }

  /* SRAM4 (secure alias) direct access check */
  volatile uint32_t *sram4 = reinterpret_cast<volatile uint32_t *>(0x38000000UL);
  uint32_t old = *sram4;
  *sram4 = 0xCAFE5A5AUL;
  bool sram4Ok = (*sram4 == 0xCAFE5A5AUL);
  *sram4 = old;

  printf("  ring buffer FIFO 512 entries OK, SRAM4 access %s\r\n", sram4Ok ? "OK" : "NG");
  return sram4Ok ? Result::Pass : Result::Fail;
}

} // namespace apptest
