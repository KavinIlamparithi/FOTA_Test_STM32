/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c   (A/B APPLICATION - wireless OTA, slot-aware)
  * @brief          : A/B jump OTA app for STM32L476RG
  *
  *  ============ BUILD MATRIX (4 binaries total) ============
  *  Each firmware VERSION is built TWICE, once per slot:
  *
  *    FW1 slot-A : MY_SLOT=1, BLINK=2000, linker ORIGIN=0x8008000
  *    FW1 slot-B : MY_SLOT=2, BLINK=2000, linker ORIGIN=0x8080000
  *    FW2 slot-A : MY_SLOT=1, BLINK=500,  linker ORIGIN=0x8008000
  *    FW2 slot-B : MY_SLOT=2, BLINK=500,  linker ORIGIN=0x8080000
  *
  *  The running app downloads the NEW firmware built for the OTHER
  *  slot, writes it there, sets the boot flag to the other slot,
  *  and reboots. The bootloader JUMPS there. No swap, no bank stall.
  *
  *  Server hosts (example for shipping FW2):
  *    /fw2_slotA.bin   (FW2 linked 0x8008000)
  *    /fw2_slotB.bin   (FW2 linked 0x8080000)
  *  Device running in slot A requests fw2_slotB.bin (writes slot B).
  *  Device running in slot B requests fw2_slotA.bin (writes slot A).
  *
  *  Wiring (hardware UART, 115200): PA9->WeMos RX, PA10->WeMos TX,
  *  WeMos 5V->Nucleo CN6-5, GND shared.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* USER CODE END Includes */

/* USER CODE BEGIN PD */

/* ===== CHANGE PER BUILD ===== */
#define MY_SLOT          2            /* 1 = slot A, 2 = slot B */
#define BLINK_RATE_MS    500
#define FW_VERSION_STR   "FW2-v2.0"
/* ============================ */

/* ===== NETWORK ===== */
#define WIFI_SSID        "WING"
#define WIFI_PASS        "asdfghjk"
#define SERVER_IP        "192.168.229.1"
#define SERVER_PORT      "8080"
/* The file to fetch = the build for the OTHER slot.
 * Edit these to match what you put on the server for each release. */
#if (MY_SLOT == 1)
  #define FW_PATH        "/fw_slotB.bin"   /* I'm in A, fetch B-build */
#else
  #define FW_PATH        "/fw_slotA.bin"   /* I'm in B, fetch A-build */
#endif
/* =================== */

/* Memory map */
#define SLOT_A_ADDR      0x08008000UL
#define SLOT_B_ADDR      0x08080000UL
#define FLAG_ADDR        0x080FF800UL
#define PAGE_SIZE        2048UL
#define SLOT_MAX         (480UL * 1024UL)
#define BOOT_MAGIC       0xB007CAFEB007CAFEULL

#if (MY_SLOT == 1)
  #define MY_ADDR        SLOT_A_ADDR
  #define OTHER_ADDR     SLOT_B_ADDR
  #define OTHER_SLOT_NUM 2
#else
  #define MY_ADDR        SLOT_B_ADDR
  #define OTHER_ADDR     SLOT_A_ADDR
  #define OTHER_SLOT_NUM 1
#endif

#define OTA_MAX_IMG      (64UL * 1024UL)
/* USER CODE END PD */

/* USER CODE BEGIN PV */
#define RXQ_SIZE  8192
static volatile uint8_t  rxq[RXQ_SIZE];
static volatile uint32_t rxq_head = 0;
static volatile uint32_t rxq_tail = 0;
static uint8_t rx_byte;

static uint8_t img_buf[OTA_MAX_IMG];
/* USER CODE END PV */

void SystemClock_Config(void);

/* USER CODE BEGIN PFP */
static void              Debug_Print(const char *msg);
static void              ESP_Send(const char *cmd);
static uint8_t           ESP_WaitFor(const char *token, uint32_t timeout_ms);
static int               RXQ_Get(uint8_t *out, uint32_t timeout_ms);
static void              RXQ_Flush(void);
static HAL_StatusTypeDef Flash_EraseRegion(uint32_t addr, uint32_t size);
static HAL_StatusTypeDef Flash_WriteImage(uint32_t addr, const uint8_t *data, uint32_t len);
static void              Boot_SetSlotAndReboot(uint32_t slot_num);
static void              OTA_Download(void);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        uint32_t next = (rxq_head + 1) % RXQ_SIZE;
        if (next != rxq_tail) { rxq[rxq_head] = rx_byte; rxq_head = next; }
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}

static int RXQ_Get(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (rxq_tail == rxq_head) {
        if ((HAL_GetTick() - start) >= timeout_ms) return 0;
    }
    *out = rxq[rxq_tail];
    rxq_tail = (rxq_tail + 1) % RXQ_SIZE;
    return 1;
}

static void RXQ_Flush(void) { rxq_tail = rxq_head; }

static void Debug_Print(const char *msg)
{
    HAL_UART_Transmit(&huart2, (const uint8_t *)msg, strlen(msg), 1000);
}

static void ESP_Send(const char *cmd)
{
    RXQ_Flush();
    HAL_UART_Transmit(&huart1, (const uint8_t *)cmd, strlen(cmd), 2000);
}

static uint8_t ESP_WaitFor(const char *token, uint32_t timeout_ms)
{
    char    win[16] = {0};
    uint8_t wlen = (uint8_t)strlen(token);
    if (wlen >= sizeof(win)) wlen = (uint8_t)(sizeof(win) - 1);
    char    errwin[6] = {0};
    uint8_t b;
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        if (!RXQ_Get(&b, 100)) continue;
        memmove(win, win + 1, wlen - 1);
        win[wlen - 1] = (char)b;
        if (memcmp(win, token, wlen) == 0) return 1;
        memmove(errwin, errwin + 1, 4);
        errwin[4] = (char)b;
        if (memcmp(errwin, "ERROR", 5) == 0) return 0;
    }
    return 0;
}

static HAL_StatusTypeDef Flash_EraseRegion(uint32_t addr, uint32_t size)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t page_err;
    HAL_StatusTypeDef st = HAL_OK;

    HAL_FLASH_Unlock();
    for (uint32_t cur = addr; cur < addr + size; cur += PAGE_SIZE) {
        e.TypeErase = FLASH_TYPEERASE_PAGES;
        e.NbPages   = 1;
        if (cur < 0x08080000UL) {
            e.Banks = FLASH_BANK_1;
            e.Page  = (cur - 0x08000000UL) / PAGE_SIZE;
        } else {
            e.Banks = FLASH_BANK_2;
            e.Page  = (cur - 0x08080000UL) / PAGE_SIZE;
        }
        st = HAL_FLASHEx_Erase(&e, &page_err);
        if (st != HAL_OK) break;
    }
    HAL_FLASH_Lock();
    return st;
}

static HAL_StatusTypeDef Flash_WriteImage(uint32_t addr, const uint8_t *data, uint32_t len)
{
    HAL_StatusTypeDef st = HAL_OK;
    uint64_t dword;
    HAL_FLASH_Unlock();
    for (uint32_t off = 0; off < len; off += 8) {
        memset(&dword, 0xFF, 8);
        uint32_t chunk = ((len - off) >= 8) ? 8 : (len - off);
        memcpy(&dword, data + off, chunk);
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + off, dword);
        if (st != HAL_OK) break;
    }
    HAL_FLASH_Lock();
    return st;
}

/* Write boot-select flag = slot_num, then reboot.
 * Flag page is in Bank 2; we run from Bank 1 (slot A) or Bank 2 (slot B).
 * If running from slot B (Bank 2) we are writing a Bank 2 page while
 * executing from Bank 2 — but this is a tiny single-page write at the
 * very end, right before reset, and we are NOT executing from the flag
 * page itself. To be safe we keep this minimal and reset immediately. */
static void Boot_SetSlotAndReboot(uint32_t slot_num)
{
    uint64_t flag[2];
    flag[0] = BOOT_MAGIC;
    flag[1] = slot_num;

    Flash_EraseRegion(FLAG_ADDR, PAGE_SIZE);
    HAL_FLASH_Unlock();
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FLAG_ADDR,      flag[0]);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FLAG_ADDR + 8U, flag[1]);
    HAL_FLASH_Lock();

    Debug_Print("Boot flag set - rebooting...\r\n");
    HAL_Delay(200);
    NVIC_SystemReset();
}

static void OTA_Download(void)
{
    char    cmd[128], dbg[96];
    uint8_t b;

    Debug_Print("\r\n--- OTA START ---\r\n");
    snprintf(dbg, sizeof(dbg), "Running slot %d, will write slot %d (%s)\r\n",
             MY_SLOT, OTHER_SLOT_NUM, FW_PATH);
    Debug_Print(dbg);

    ESP_Send("AT\r\n");
    if (!ESP_WaitFor("OK", 2000)) { Debug_Print("AT fail\r\n"); return; }
    Debug_Print("AT ok\r\n");

    ESP_Send("AT+CWMODE=1\r\n");
    ESP_WaitFor("OK", 2000);

    Debug_Print("Joining WiFi...\r\n");
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    ESP_Send(cmd);
    if (!ESP_WaitFor("OK", 20000)) { Debug_Print("WiFi join fail\r\n"); return; }
    Debug_Print("WiFi joined\r\n");

    ESP_Send("AT+CIPMUX=0\r\n");
    ESP_WaitFor("OK", 2000);

    Debug_Print("TCP connecting...\r\n");
    snprintf(cmd, sizeof(cmd),
             "AT+CIPSTART=\"TCP\",\"%s\",%s\r\n", SERVER_IP, SERVER_PORT);
    ESP_Send(cmd);
    if (!ESP_WaitFor("CONNECT", 8000)) { Debug_Print("TCP fail\r\n"); return; }
    Debug_Print("TCP connected\r\n");

    char req[160];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             FW_PATH, SERVER_IP);
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u\r\n", (unsigned)strlen(req));
    ESP_Send(cmd);
    if (!ESP_WaitFor(">", 3000)) { Debug_Print("CIPSEND no prompt\r\n"); return; }

    HAL_UART_Transmit(&huart1, (uint8_t *)req, strlen(req), 2000);
    if (!ESP_WaitFor("SEND OK", 3000)) { Debug_Print("SEND fail\r\n"); return; }
    Debug_Print("GET sent, receiving into RAM...\r\n");

    enum { S_FIND_IPD, S_IPD_LEN, S_IPD_DATA } state = S_FIND_IPD;
    char     ipd_win[5] = {0};
    uint32_t ipd_len = 0;
    uint8_t  in_headers = 1;
    char     hdr_end[4] = {0};
    char     hdr_line[80];
    uint8_t  hdr_li = 0;
    uint32_t content_len = 0;
    uint32_t body_len = 0;
    uint8_t  overflow = 0;

    while (1) {
        if (!RXQ_Get(&b, 5000)) { Debug_Print("RX idle - end of stream\r\n"); break; }

        if (state == S_FIND_IPD) {
            memmove(ipd_win, ipd_win + 1, 4);
            ipd_win[4] = (char)b;
            if (memcmp(ipd_win, "+IPD,", 5) == 0) { ipd_len = 0; state = S_IPD_LEN; }
            continue;
        }
        if (state == S_IPD_LEN) {
            if (b == ':') state = S_IPD_DATA;
            else if (b >= '0' && b <= '9') ipd_len = ipd_len * 10u + (uint32_t)(b - '0');
            continue;
        }
        if (in_headers) {
            if (b == '\n') {
                if (hdr_li > 0 && hdr_line[hdr_li - 1] == '\r') hdr_li--;
                hdr_line[hdr_li] = '\0'; hdr_li = 0;
                char *cl = strstr(hdr_line, "Content-Length:");
                if (!cl) cl = strstr(hdr_line, "content-length:");
                if (cl) { cl += 15; while (*cl == ' ') cl++; content_len = (uint32_t)atoi(cl); }
            } else if (hdr_li < (uint8_t)(sizeof(hdr_line) - 1)) {
                hdr_line[hdr_li++] = (char)b;
            }
            hdr_end[0]=hdr_end[1]; hdr_end[1]=hdr_end[2];
            hdr_end[2]=hdr_end[3]; hdr_end[3]=(char)b;
            if (hdr_end[0]=='\r'&&hdr_end[1]=='\n'&&hdr_end[2]=='\r'&&hdr_end[3]=='\n') {
                in_headers = 0;
                Debug_Print("Headers done, buffering body...\r\n");
            }
        } else {
            if (body_len < OTA_MAX_IMG) img_buf[body_len++] = b;
            else overflow = 1;
        }
        if (ipd_len > 0 && --ipd_len == 0) state = S_FIND_IPD;
    }

    ESP_Send("AT+CIPCLOSE\r\n");
    ESP_WaitFor("OK", 2000);

    snprintf(dbg, sizeof(dbg), "Received %lu bytes (Content-Length: %lu)\r\n",
             (unsigned long)body_len, (unsigned long)content_len);
    Debug_Print(dbg);

    if (overflow)      { Debug_Print("Image too big!\r\n"); return; }
    if (body_len == 0) { Debug_Print("No firmware received!\r\n"); return; }
    if (content_len > 0 && body_len != content_len) {
        Debug_Print("Size mismatch - ABORT\r\n"); return;
    }

    /* Write the downloaded image into the OTHER slot */
    snprintf(dbg, sizeof(dbg), "Erasing slot %d...\r\n", OTHER_SLOT_NUM);
    Debug_Print(dbg);
    if (Flash_EraseRegion(OTHER_ADDR, SLOT_MAX) != HAL_OK) {
        Debug_Print("Erase fail\r\n"); return;
    }
    Debug_Print("Writing image...\r\n");
    if (Flash_WriteImage(OTHER_ADDR, img_buf, body_len) != HAL_OK) {
        Debug_Print("Write fail\r\n"); return;
    }

    /* Validate the slot we just wrote actually has a sane vector table */
    uint32_t sp = *(volatile uint32_t *)OTHER_ADDR;
    if (!(sp >= 0x20000000UL && sp <= 0x20018000UL)) {
        Debug_Print("Written image invalid (bad SP) - NOT switching\r\n");
        return;
    }

    Debug_Print("Image OK. Switching slots.\r\n");
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    Boot_SetSlotAndReboot(OTHER_SLOT_NUM);   /* reboots */
}

/* USER CODE END 0 */

int main(void)
{
    /* USER CODE BEGIN 1 */
    SCB->VTOR = MY_ADDR;
    /* USER CODE END 1 */

    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    /* USER CODE BEGIN 2 */
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    g.Pin = GPIO_PIN_5; g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_13; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &g);

    HAL_UART_Receive_IT(&huart1, &rx_byte, 1);

    char banner[96];
    snprintf(banner, sizeof(banner),
             "\r\n=== %s | slot %c | blink %lu ms ===\r\n",
             FW_VERSION_STR, (MY_SLOT == 1 ? 'A' : 'B'),
             (unsigned long)BLINK_RATE_MS);
    Debug_Print(banner);
    Debug_Print("Press B1 to start wireless OTA.\r\n");

    while (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET)
        HAL_Delay(10);
    /* USER CODE END 2 */

    while (1)
    {
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
        HAL_Delay(BLINK_RATE_MS);

        if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET) {
            HAL_Delay(50);
            if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET) {
                while (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET)
                    HAL_Delay(10);
                OTA_Download();
                Debug_Print("OTA aborted - resuming.\r\n");
            }
        }
    }
}

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

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK
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