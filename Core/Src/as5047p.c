#include "as5047p.h"
#include "main.h"
#include "current_sense.h"

#include <ctype.h>
#include <stdio.h>

#define AS5047P_REPORT_PERIOD_MS 100U
#define AS5047P_SPI_TIMEOUT_MS   2U
#define AS5047P_REG_ERRFL        0x0001U
#define AS5047P_REG_DIAAGC       0x3FFCU
#define AS5047P_REG_ANGLECOM     0x3FFFU
#define AS5047P_READ             0x4000U
#define AS5047P_PARITY           0x8000U
#define AS5047P_DATA_MASK        0x3FFFU
#define AS5047P_DIAG_LF          0x0100U
#define AS5047P_DIAG_ERRORS      0x0E00U

typedef enum
{
  READ_OK,
  READ_SPI_ERROR,
  READ_PARITY_ERROR,
  READ_SENSOR_ERROR
} ReadResult;

static SPI_HandleTypeDef *encoder_spi;
static uint32_t report_tick;
static bool streaming;

static void ReadAndPrintSample(bool initial_check);

static bool OddParity(uint16_t word)
{
  word ^= word >> 8;
  word ^= word >> 4;
  word ^= word >> 2;
  word ^= word >> 1;
  return (word & 1U) != 0U;
}

static void DelayOneMicrosecond(void)
{
  const uint32_t cycles = (SystemCoreClock + 999999U) / 1000000U;
  const uint32_t start = DWT->CYCCNT;
  while ((uint32_t)(DWT->CYCCNT - start) < cycles)
  {
    /* Leave interrupts enabled. */
  }
}

static HAL_StatusTypeDef TransferFrame(uint16_t tx, uint16_t *rx)
{
  HAL_StatusTypeDef status;
  HAL_GPIO_WritePin(SPI1_SS_GPIO_Port, SPI1_SS_Pin, GPIO_PIN_RESET);
  /* Datasheet: CS setup >=350 ns. At 5 MHz CS hold >=100 ns,
   * and CS high between frames >=350 ns. Use 1 us for each.
   */
  DelayOneMicrosecond();
  /* HAL Size counts 16-bit words here, not bytes. HAL waits for BSY clear. */
  status = HAL_SPI_TransmitReceive(encoder_spi, (uint8_t *)&tx,
                                   (uint8_t *)rx, 1U, AS5047P_SPI_TIMEOUT_MS);
  DelayOneMicrosecond();
  HAL_GPIO_WritePin(SPI1_SS_GPIO_Port, SPI1_SS_Pin, GPIO_PIN_SET);
  DelayOneMicrosecond();
  return status;
}

static ReadResult ReadRegister(uint16_t address, uint16_t *data)
{
  uint16_t command = AS5047P_READ | address;
  uint16_t response = 0U;
  if (OddParity(command))
  {
    command |= AS5047P_PARITY;
  }
  /* First response belongs to the previous command. NOP clocks out this
   * read's response in a separate CS frame (pipelined protocol).
   */
  if ((TransferFrame(command, &response) != HAL_OK) ||
      (TransferFrame(0x0000U, &response) != HAL_OK))
  {
    return READ_SPI_ERROR;
  }
  if (OddParity(response))
  {
    return READ_PARITY_ERROR;
  }
  *data = response & AS5047P_DATA_MASK;
  return (response & AS5047P_READ) ? READ_SENSOR_ERROR : READ_OK;
}

static void PrintReadError(ReadResult result)
{
  if (result == READ_SPI_ERROR)
  {
    printf("AS5047P SPI error: HAL error=0x%08lX\r\n",
           (unsigned long)HAL_SPI_GetError(encoder_spi));
  }
  else if (result == READ_PARITY_ERROR)
  {
    printf("AS5047P RX parity error (check wiring/SPI)\r\n");
  }
  else
  {
    uint16_t flags = 0U;
    /* Reading ERRFL acknowledges communication errors for the next sample.
     * The response may still carry EF; accept its data only with valid parity.
     */
    ReadResult error_result = ReadRegister(AS5047P_REG_ERRFL, &flags);
    if ((error_result == READ_OK) || (error_result == READ_SENSOR_ERROR))
    {
      printf("AS5047P EF=1, ERRFL=0x%04X [PARERR=%u INVCOMM=%u FRERR=%u]\r\n",
             (unsigned int)flags, (flags >> 2) & 1U,
             (flags >> 1) & 1U, flags & 1U);
    }
    else
    {
      printf("AS5047P EF=1, ERRFL read failed (%s)\r\n",
             error_result == READ_SPI_ERROR ? "SPI" : "parity");
    }
  }
}

void AS5047P_Init(SPI_HandleTypeDef *spi)
{
  encoder_spi = spi;
  /* Share the cycle counter with motor-control timing; never reset it. */
  SET_BIT(CoreDebug->DEMCR, CoreDebug_DEMCR_TRCENA_Msk);
  SET_BIT(DWT->CTRL, DWT_CTRL_CYCCNTENA_Msk);
  HAL_GPIO_WritePin(SPI1_SS_GPIO_Port, SPI1_SS_Pin, GPIO_PIN_SET);
  report_tick = HAL_GetTick();
  streaming = false;
  /* Allow the sensor's power-on settling time before the one-shot check. */
  HAL_Delay(10U);
  ReadAndPrintSample(true);
}

static bool CommandEquals(const char *text, const char *expected)
{
  while (isspace((unsigned char)*text))
  {
    text++;
  }
  while (*expected != '\0')
  {
    if (tolower((unsigned char)*text) != *expected++)
    {
      return false;
    }
    text++;
  }
  while (isspace((unsigned char)*text))
  {
    text++;
  }
  return *text == '\0';
}

bool AS5047P_ProcessCommand(const char *command)
{
  if (command == NULL)
  {
    return false;
  }
  if (CommandEquals(command, "angle") || CommandEquals(command, "angle start"))
  {
    streaming = true;
    report_tick = HAL_GetTick() - AS5047P_REPORT_PERIOD_MS;
    printf("AS5047P stream started: ~100 ms interval; 'angle stop' to stop\r\n");
    return true;
  }
  if (CommandEquals(command, "angle stop"))
  {
    streaming = false;
    printf("AS5047P stream stopped\r\n");
    return true;
  }
  return false;
}

static void ReadAndPrintSample(bool initial_check)
{
  uint16_t diagnostic = 0U;
  uint16_t angle = 0U;
  ReadResult result;
  result = ReadRegister(AS5047P_REG_DIAAGC, &diagnostic);
  if (result != READ_OK)
  {
    if (initial_check)
    {
      printf("AS5047P INIT ERROR: DIAAGC read failed\r\n");
    }
    PrintReadError(result);
    return;
  }
  /* Reject not-ready, CORDIC overflow and out-of-range magnetic field.
   * In particular, a stuck-low MISO must not look like a valid zero angle.
   */
  if ((diagnostic & (AS5047P_DIAG_LF | AS5047P_DIAG_ERRORS)) != AS5047P_DIAG_LF)
  {
    printf("AS5047P %s: DIAAGC=0x%04X [LF=%u COF=%u MAGH=%u MAGL=%u]\r\n",
           initial_check ? "INIT ERROR" : "invalid",
           (unsigned int)diagnostic, (diagnostic >> 8) & 1U,
           (diagnostic >> 9) & 1U, (diagnostic >> 10) & 1U,
           (diagnostic >> 11) & 1U);
    return;
  }
  result = ReadRegister(AS5047P_REG_ANGLECOM, &angle);
  if (result != READ_OK)
  {
    if (initial_check)
    {
      printf("AS5047P INIT ERROR: ANGLECOM read failed\r\n");
    }
    PrintReadError(result);
    return;
  }
  if (initial_check)
  {
    printf("AS5047P INIT OK: DIAAGC=0x%04X, raw=%u, angle=%.2f deg\r\n",
           (unsigned int)diagnostic, (unsigned int)angle,
           (double)angle * (360.0 / 16384.0));
    return;
  }
  printf("AS5047P raw=%u, angle=%.2f deg\r\n", (unsigned int)angle,
         (double)angle * (360.0 / 16384.0));
}

void AS5047P_Task(void)
{
  uint32_t now = HAL_GetTick();
  if ((encoder_spi == NULL) || !streaming || CurrentSense_IsBusy() ||
      ((uint32_t)(now - report_tick) < AS5047P_REPORT_PERIOD_MS))
  {
    return;
  }
  report_tick = now;
  ReadAndPrintSample(false);
}
