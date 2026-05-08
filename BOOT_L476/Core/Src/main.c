/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c   (A/B JUMP BOOTLOADER - BOOT_L476)
  * @brief          : Slot-select jump bootloader for STM32L476RG
  *
  *  WHY THIS DESIGN (vs swap):
  *    The bootloader does NOTHING risky at boot — no erase, no write,
  *    no copy. It only reads a flag and JUMPS to slot A or slot B.
  *    Because it never erases the bank it executes from, it cannot
  *    stall/brick during boot. This is the robust foundation for a
  *    no-brick product. (The swap design kept bricking on the L476's
  *    "erase the bank you run from" stall — this avoids that entirely.)
  *
  *  Memory map:
  *    0x08000000  Bootloader (32KB)
  *    0x08008000  SLOT A   (Bank 1)  — FW linked for THIS address
  *    0x08080000  SLOT B   (Bank 2)  — FW linked for THIS address
  *    0x080FF800  Boot-select flag (Bank 2 last page)
  *
  *  Flag logic:
  *    magic==BOOT_MAGIC && slot==2  -> jump SLOT B
  *    otherwise                     -> jump SLOT A
  *    chosen slot invalid           -> fall back to the other
  *    neither valid                 -> fast error blink
  *
  *  NOTE: each firmware must be built TWICE (once per slot address).
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */
#include <string.h>
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
#define SLOT_A_ADDR   0x08008000UL
#define SLOT_B_ADDR   0x08080000UL
#define FLAG_ADDR     0x080FF800UL
#define BOOT_MAGIC    0xB007CAFEB007CAFEULL
/* USER CODE END PD */

void SystemClock_Config(void);

/* USER CODE BEGIN 0 */

typedef struct __attribute__((packed)) {
    uint64_t magic;     /* BOOT_MAGIC when valid */
    uint64_t slot;      /* 1 = SLOT A, 2 = SLOT B */
} boot_select_t;

/* A slot is bootable if its first word (initial SP) points into SRAM */
static uint8_t Slot_Valid(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *)addr;
    return (sp >= 0x20000000UL) && (sp <= 0x20018000UL);
}

static void LED_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin  = GPIO_PIN_5;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOA, &g);
}

static void Error_Blink(void)
{
    LED_Init();
    while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5); HAL_Delay(100); }
}

static void Jump_To(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *)(addr);
    uint32_t pc = *(volatile uint32_t *)(addr + 4U);

    HAL_RCC_DeInit();
    HAL_DeInit();
    SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;
    for (int i = 0; i < 8; i++) { NVIC->ICER[i] = 0xFFFFFFFFUL; NVIC->ICPR[i] = 0xFFFFFFFFUL; }

    SCB->VTOR = addr;
    __set_MSP(sp);
    ((void (*)(void))pc)();
}

/* USER CODE END 0 */

int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  HAL_Init();
  SystemClock_Config();

  /* USER CODE BEGIN 2 */
  boot_select_t flag;
  memcpy(&flag, (const void *)FLAG_ADDR, sizeof(flag));

  /* Default SLOT A; flag picks SLOT B only if valid + slot==2 */
  uint32_t target = SLOT_A_ADDR;
  if (flag.magic == BOOT_MAGIC && flag.slot == 2) {
      target = SLOT_B_ADDR;
  }

  /* Fall back to the other slot if the chosen one is empty/corrupt */
  if (!Slot_Valid(target)) {
      target = (target == SLOT_A_ADDR) ? SLOT_B_ADDR : SLOT_A_ADDR;
  }

  if (!Slot_Valid(target)) {
      Error_Blink();   /* neither slot bootable */
  }

  Jump_To(target);
  /* USER CODE END 2 */

  while (1) {}
}

/**
  * @brief System Clock Configuration
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
    Error_Handler();

  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM            = 1;
  RCC_OscInitStruct.PLL.PLLN            = 10;
  RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ            = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR            = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    Error_Handler();

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    Error_Handler();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif