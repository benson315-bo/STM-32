/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 30秒或輸錯5次雙燈恆亮（PC13直接關燈）
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <string.h>   // strlen()、strcmp()
#include <stdio.h>    // snprintf()
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */
/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */
/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */
/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */
/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim3;
UART_HandleTypeDef huart2;
/* USER CODE BEGIN PV */
uint8_t rx;
char password[32];
uint8_t index_rx = 0;
volatile uint8_t button_event = 0;
volatile uint32_t button_tick = 0;
uint8_t alarm_on = 1;
uint8_t errors = 0;
uint8_t duty = 0;
uint8_t direction = 1;
uint32_t blink_ms = 500;
uint32_t blink_tick = 0;
uint32_t breath_tick = 0;
/* HAL_GetTick()單位為毫秒，30000ms = 30秒。
   challenge_timeout=1代表逾時且雙燈恆亮；
   alarm_on=0代表一般閃爍/呼吸不再執行。 */
#define CHALLENGE_TIMEOUT_MS 30000UL
uint32_t challenge_start_tick = 0;
uint8_t challenge_timeout = 0;
uint8_t attempts_exhausted = 0; /* 錯5次鎖定 */
uint8_t buzzer_active = 0;
uint32_t buzzer_start_tick = 0;
/* USER CODE END PV */
/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */
/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* 本程式使用：
   PC8：外接警示LED，GPIO輸出。
   PC6：TIM3_CH1，PWM控制外接LED。
   PC13：USER按鈕，EXTI下降緣中斷。
   USART2：115200、8N1，透過ST-LINK虛擬COM輸入密碼。
   注意：外部中斷處理函式和TIM3/USART2底層腳位配置
   仍需保留CubeIDE專案其他自動產生的檔案。
   PWM腳位於本檔直接設定為PC6，不呼叫原本的PA6 PostInit。 */
/* PB1 控制有源蜂鳴器（僅提供控制訊號，接線須符合電氣規格）。 */
void BuzzerStop(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
    buzzer_active = 0;
}

void BuzzerStart2s(void)
{
    buzzer_start_tick = HAL_GetTick();
    buzzer_active = 1;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);
}

void BuzzerUpdate(void)
{
    if (buzzer_active &&
        (uint32_t)(HAL_GetTick() - buzzer_start_tick) >= 2000UL)
    {
        BuzzerStop();
    }
}

void Send(char *s)
{
    HAL_UART_Transmit(&huart2,
        (uint8_t *)s, strlen(s), 100);
}
/* TIM3_CH1 PWM占空比：ARR=999，percent*10約對應0~100%。
   PC6接外部LED（需串聯限流電阻），數值越大越亮。 */
void SetPWM(uint8_t percent)
{
    if (percent > 100) percent = 100;
    __HAL_TIM_SET_COMPARE(&htim3,
        TIM_CHANNEL_1, percent * 10);
}
/* 停止挑戰：alarm_on=0 使主迴圈不再閃爍或呼吸；
   PC8 輸出低電位、PC6 PWM 占空比設為0，兩顆燈熄滅。
   PC13 按下與正確密碼都會呼叫此函式。 */
void StopAlarm(void)
{
    BuzzerStop();
    alarm_on = 0;
    challenge_timeout = 0;
    attempts_exhausted = 0;
    duty = 0;
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8,
                      GPIO_PIN_RESET);
    SetPWM(0);
}
/* 初始化挑戰：重設錯誤次數、呼吸方向、閃爍間隔；
   記錄 HAL_GetTick() 當作30秒倒數起點。
   本版本只在開機時呼叫，不會由 PC13 重新啟動。 */
void StartAlarm(void)
{
    BuzzerStop();
    alarm_on = 1;
    challenge_timeout = 0;
    attempts_exhausted = 0;
    challenge_start_tick = HAL_GetTick();
    errors = 0;
    duty = 0;
    direction = 1;
    blink_ms = 500;
    blink_tick = HAL_GetTick();
    breath_tick = HAL_GetTick();
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8,
                      GPIO_PIN_RESET);
    SetPWM(0);
}
/* 30秒逾時：關閉一般動畫更新，讓PC8固定高電位；
   PC6 PWM 設100%占空比，因此外接LED持續亮著。
   仍可透過PC13呼叫StopAlarm()使兩顆燈熄滅。 */
void EnterTimeoutState(void)
{
    challenge_timeout = 1;
    alarm_on = 0; /* 停止原本的閃爍與呼吸更新 */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);
    SetPWM(100);
    BuzzerStart2s();
    Send("TIMEOUT:Both LEDs ON\r\n");
}
/* 輸錯5次時，兩顆LED固定全亮；只有PC13或RESET可停止。 */
void EnterWrongLimitState(void)
{
    attempts_exhausted = 1;
    alarm_on = 0;
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);
    SetPWM(100);
    BuzzerStart2s();
    Send("FAILED:5 wrong attempts,Both LEDs ON\r\n");
}
/* UART 收到換行後檢查密碼：
   1. 逾時：拒絕再解除；2. 停止：回報已關閉；
   3. 正確1234：兩燈熄滅；4. 錯誤：加快PC8、提高PC6亮度上限。 */
void CheckPassword(void)
{
    char msg[80];
    if (challenge_timeout)
    {
        Send("TIMEOUT:Press PC13 to stop\r\n");
    }
    else if (attempts_exhausted)
    {
        Send("FAILED:Press PC13 to stop\r\n");
    }
    else if (!alarm_on)
    {
        Send("Alarm is OFF\r\n");
    }
    else if (!strcmp(password, "1234"))
    {
        StopAlarm();
        Send("OK:Alarm OFF\r\n");
    }
    else if (alarm_on)
    {
        if (errors < 5) errors++;
        blink_ms = errors >= 4
                 ? 100 : 500 - errors * 100;
        snprintf(msg, sizeof(msg),
            "WRONG:%lu,DUTY:%u,COUNT:%u\r\n",
            (unsigned long)blink_ms,
            errors * 20, errors);
        Send(msg);
        /* 第五次輸錯立即固定雙燈全亮 */
        if (errors >= 5)
        {
            EnterWrongLimitState();
        }
    }
    else
    {
        Send("Alarm is OFF\r\n");
    }
}
/* PC13 下降緣外部中斷：200ms 防彈跳。
   中斷中只設定 button_event，避免在中斷內執行UART傳送。 */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    uint32_t now = HAL_GetTick();
    if (pin == GPIO_PIN_13 &&
        (uint32_t)(now - button_tick) >= 200)
    {
        button_tick = now;
        button_event = 1;
    }
}
/* USER CODE END 0 */
/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */
  /* MCU Configuration--------------------------------------------------------*/
  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */
  /* Configure the system clock */
  SystemClock_Config();
  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */
  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM3_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
  /* 初始化警報、呼吸燈與30秒倒數起點 */
  StartAlarm();
  Send("STM32 Alarm Ready\r\n");
  Send("30-second challenge started\r\n");
  /* USER CODE END 2 */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
	  if (button_event)
	  {
	      button_event = 0;
          /* PC13 僅能停止，不會再次啟動挑戰 */
          StopAlarm();
          Send("Button: Alarm OFF\r\n");
	  }
	  /* USART2 非阻塞輪詢：每次取1 byte；遇到換行才驗證。
       使用32-byte緩衝區，超長輸入會清空並回報。 */
	  if (HAL_UART_Receive(&huart2, &rx, 1, 0)
	      == HAL_OK)
	  {
	      if (rx == '\r' || rx == '\n')
	      {
	          if (index_rx)
	          {
	              password[index_rx] = '\0';
	              if (alarm_on && !challenge_timeout &&
	                  (uint32_t)(HAL_GetTick() - challenge_start_tick) >= CHALLENGE_TIMEOUT_MS)
	              {
	                  EnterTimeoutState();
	              }
	              CheckPassword();
	              index_rx = 0;
	          }
	      }
	      else if (index_rx < sizeof(password) - 1)
	      {
	          password[index_rx++] = rx;
	      }
	      else
	      {
	          index_rx = 0;
	          Send("Input too long\r\n");
	      }
	  }
	  /* 30秒倒數：用HAL_GetTick()的毫秒差判斷是否到時；
       不用HAL_Delay()，因此期間仍可收密碼與處理PC13。 */
	  uint32_t now = HAL_GetTick();
	  if (alarm_on && !challenge_timeout &&
	      (uint32_t)(now - challenge_start_tick) >= CHALLENGE_TIMEOUT_MS)
	  {
	      EnterTimeoutState();
	  }
	  /* alarm_on為1才更新燈號；正確密碼、PC13或逾時後均停止。
       PC8依blink_ms翻轉；PC6每20ms調整PWM形成呼吸效果。 */
	  if (alarm_on)
	  {
	      if (now - blink_tick >= blink_ms)
	      {
	          blink_tick = now;
	          HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_8);
	      }
	      if (alarm_on && errors > 0 && now - breath_tick >= 20)
	          {
	              breath_tick = now;
	              if (direction)
	              {
	                  if (duty < 100) duty++;
	                  if (duty >= 100) direction = 0;
	              }
	              else
	              {
	                  if (duty > 0) duty--;
	                  if (duty == 0) direction = 1;
	              }
	              uint8_t max_duty =
	                  (errors >= 5) ? 100 : errors * 20;
	              uint8_t actual_duty =
	                  5 + (uint16_t)duty * (max_duty - 5) / 100;
	              SetPWM(actual_duty);
	          }
	  }
    BuzzerUpdate();
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}
/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}
/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{
  /* USER CODE BEGIN TIM3_Init 0 */
  /* USER CODE END TIM3_Init 0 */
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  /* USER CODE BEGIN TIM3_Init 1 */
  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 169;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */
  /* USER CODE END TIM3_Init 2 */
  /* PC6 = TIM3_CH1 (AF2). Configure here instead of the old PA6 PostInit. */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef PWM_GPIO_InitStruct = {0};
    PWM_GPIO_InitStruct.Pin = GPIO_PIN_6;
    PWM_GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    PWM_GPIO_InitStruct.Pull = GPIO_NOPULL;
    PWM_GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    PWM_GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOC, &PWM_GPIO_InitStruct);
}
/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{
  /* USER CODE BEGIN USART2_Init 0 */
  /* USER CODE END USART2_Init 0 */
  /* USER CODE BEGIN USART2_Init 1 */
  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart2, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart2, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */
  /* USER CODE END USART2_Init 2 */
}
/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */
  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
  /*Configure GPIO pin : PC8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
  /* PB1: 有源蜂鳴器控制 GPIO */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
