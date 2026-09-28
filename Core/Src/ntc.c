#include "ntc.h"
#include "current_sense.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>

#define NTC_SAMPLE_PERIOD_MS 10U
#define NTC_REPORT_PERIOD_MS 1000U
#define NTC_PULLDOWN_OHMS 33000.0f

/* NCP18XH103F03RB: R25=10k, B25/50=3380K (constant-B approximation).
 * 3.3V -- NTC -- PC4 -- 33k -- GND; 33nF from PC4 to GND.
 * Requires VREF+ to equal the divider supply voltage.
 */
static ADC_HandleTypeDef *ntc_adc;
static uint32_t sample_tick;
static uint32_t report_tick;
static bool streaming;

void NTC_Init(ADC_HandleTypeDef *adc)
{
  ntc_adc = adc;
  sample_tick = HAL_GetTick();
  report_tick = sample_tick;
  streaming = false;
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

bool NTC_ProcessCommand(const char *command)
{
  if (command == NULL)
  {
    return false;
  }
  if (CommandEquals(command, "ntc") || CommandEquals(command, "ntc start"))
  {
    streaming = true;
    report_tick = HAL_GetTick() - NTC_REPORT_PERIOD_MS;
    printf("NTC stream started: ~1 s interval; 'ntc stop' to stop\r\n");
    return true;
  }
  if (CommandEquals(command, "ntc stop"))
  {
    streaming = false;
    printf("NTC stream stopped\r\n");
    return true;
  }
  return false;
}

void NTC_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint16_t raw = 0U;
  HAL_StatusTypeDef status = HAL_OK;

  if ((ntc_adc == NULL) || CurrentSense_IsBusy() ||
      ((uint32_t)(now - sample_tick) < NTC_SAMPLE_PERIOD_MS))
  {
    return;
  }
  sample_tick = now;

  /* ES0431 2.5.9: discard first conversion after >1 ms ADC inactivity.
   * Both conversions finish here before main can start an injected capture.
   * Interrupts remain enabled. Never stop the shared ADC with HAL_ADC_Stop().
   */
  for (uint32_t i = 0U; i < 2U; i++)
  {
    status = HAL_ADC_Start(ntc_adc);
    if (status != HAL_OK)
    {
      break;
    }
    status = HAL_ADC_PollForConversion(ntc_adc, 1U);
    if (status != HAL_OK)
    {
      break;
    }
    raw = (uint16_t)HAL_ADC_GetValue(ntc_adc);
  }
  if (status != HAL_OK)
  {
    (void)HAL_ADCEx_RegularStop(ntc_adc);
  }

  if (!streaming || ((uint32_t)(now - report_tick) < NTC_REPORT_PERIOD_MS))
  {
    return;
  }
  report_tick = now;

  if (status != HAL_OK)
  {
    printf("NTC ADC error: HAL status=%d\r\n", (int)status);
  }
  else if ((raw == 0U) || (raw >= 4095U))
  {
    printf("NTC raw=%u, invalid: ADC rail (check sensor/wiring)\r\n",
           (unsigned int)raw);
  }
  else
  {
    float resistance = NTC_PULLDOWN_OHMS * (4095.0f / (float)raw - 1.0f);
    float temperature = 1.0f /
        (1.0f / 298.15f + logf(resistance / 10000.0f) / 3380.0f) - 273.15f;
    printf("NTC raw=%u, R=%.0f ohm, T=%.2f C%s\r\n",
           (unsigned int)raw, (double)resistance, (double)temperature,
           ((temperature < -40.0f) || (temperature > 125.0f))
               ? " (outside sensor range)" : "");
  }
}
