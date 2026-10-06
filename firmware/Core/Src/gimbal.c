/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gimbal.c
  * @brief   二自由度云台控制模块实现
  *
  * 数据流：
  *   USART1(115200) -> HAL_UART_RxCpltCallback -> Gimbal_ParseByte
  *                  -> GimbalFrame_t -> Gimbal_ApplyFrame -> TIM1_CH1/CH2 PWM
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gimbal.h"
#include "tim.h"
#include "usart.h"

/* Private define ------------------------------------------------------------*/
#define GIMBAL_RX_BUF_SIZE   40U   /* 单条指令最大长度 */

/* 调试回显开关：置 1 时每收到一帧都会通过 USART1 回显原始报文与解析结果。
 * 注意：回显在串口中断里执行，会占用约 1~2ms，正式使用时保持 0。
 * 调试时可以置 1，用串口助手直接看到主板收到了什么。 */
#ifndef GIMBAL_DEBUG_ECHO
#define GIMBAL_DEBUG_ECHO    0
#endif

/* Private variables ---------------------------------------------------------*/
static GimbalState_t s_state = GIMBAL_ST_WAIT_HEAD;   /* 解析状态机当前状态 */
static uint8_t  s_is_gimbal_frame = 0;                /* 当前帧是 'g' 帧还是 's' 帧 */
static uint8_t  s_axis = 0;                           /* 正在接收的轴号 */
static uint16_t s_angle = 0;                          /* 正在接收的角度（临时累加值） */
static uint8_t  s_digit_count = 0;                    /* 已接收的数字位数，用于限长 */

static GimbalFrame_t s_frame;                         /* 当前正在拼装的帧 */

static uint8_t  s_axis_angle[GIMBAL_AXIS_COUNT] = { GIMBAL_ANGLE_MID, GIMBAL_ANGLE_MID };

#if (GIMBAL_DEBUG_ECHO == 1)
static uint8_t  s_rx_buf[GIMBAL_RX_BUF_SIZE];         /* 原始报文缓存，仅调试回显用 */
static uint8_t  s_rx_len = 0;
#endif

static uint32_t s_last_byte_tick = 0;                 /* 最近一次收字节的时刻 */

/* Private function prototypes -----------------------------------------------*/
static uint8_t  Gimbal_CommitFrame(void);
static uint8_t  Gimbal_IsHeaderByte(uint8_t byte);
#if (GIMBAL_DEBUG_ECHO == 1)
static void     Gimbal_UartPutByte(uint8_t byte);
static void     Gimbal_WriteLog(const char *text);
static void     Gimbal_WriteLogFrame(void);
static void     Gimbal_WriteLogText(const char *text);
static void     Gimbal_WriteU32(uint32_t value);
#endif

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  判断字节是否为报文帧头
  * @param  byte: 待判断字节
  * @retval 1 = 'g'/'G'/'s'/'S'，是帧头
  */
static uint8_t Gimbal_IsHeaderByte(uint8_t byte)
{
    return (uint8_t)((byte == 'g' || byte == 'G' || byte == 's' || byte == 'S') ? 1U : 0U);
}

/**
  * @brief  复位解析状态机与行缓冲
  */
void Gimbal_ResetParser(void)
{
    s_state = GIMBAL_ST_WAIT_HEAD;
    s_is_gimbal_frame = 0;
    s_axis = 0;
    s_angle = 0;
    s_digit_count = 0;
#if (GIMBAL_DEBUG_ECHO == 1)
    s_rx_len = 0;
#endif
    s_frame.target_mask = 0;
    s_frame.angle[0] = GIMBAL_ANGLE_MID;
    s_frame.angle[1] = GIMBAL_ANGLE_MID;
}

/**
  * @brief  提交当前拼装完成的帧并应用到舵机
  * @retval 1 = 帧有效且已应用；0 = 空帧，已丢弃
  */
static uint8_t Gimbal_CommitFrame(void)
{
    GimbalFrame_t frame = s_frame;

    if (frame.target_mask == 0U)
    {
#if (GIMBAL_DEBUG_ECHO == 1)
        Gimbal_WriteLogText("[gimbal] ERR: empty frame\r\n");
#endif
        Gimbal_ResetParser();
        return 0U;
    }

    /* 's' 单舵机帧只更新指定轴，另一轴保持当前角度不动 */
    if (s_is_gimbal_frame == 0U)
    {
        if ((frame.target_mask & 0x01U) == 0U)
        {
            frame.angle[0] = s_axis_angle[0];
            frame.target_mask |= 0x01U;
        }
        if ((frame.target_mask & 0x02U) == 0U)
        {
            frame.angle[1] = s_axis_angle[1];
            frame.target_mask |= 0x02U;
        }
    }

#if (GIMBAL_DEBUG_ECHO == 1)
    Gimbal_WriteLogFrame();
    Gimbal_WriteLog("[gimbal] OK");
#endif

    Gimbal_ApplyFrame(&frame);
    Gimbal_ResetParser();
    return 1U;
}

#if (GIMBAL_DEBUG_ECHO == 1)
/**
  * @brief  安全的阻塞式串口发送
  * @param  byte: 待发送字节
  * @note   等待 TXE 时带超时保护，万一串口被关闭也不会把中断卡死。
  */
static void Gimbal_UartPutByte(uint8_t byte)
{
    uint32_t guard = 200000U;

    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TXE) == RESET)
    {
        while ((__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TXE) == RESET) && (guard > 0U))
        {
            guard--;
        }
    }

    if (guard > 0U)
    {
        huart1.Instance->DR = byte;
    }
}

/**
  * @brief  通过 USART1 输出一段文本（仅调试用）
  */
static void Gimbal_WriteLogText(const char *text)
{
    while (*text != '\0')
    {
        Gimbal_UartPutByte((uint8_t)(*text++));
    }
}

/**
  * @brief  通过 USART1 输出无符号整数的十进制文本
  */
static void Gimbal_WriteU32(uint32_t value)
{
    char tmp[11];
    int  i = 0;

    if (value == 0U)
    {
        Gimbal_WriteLogText("0");
        return;
    }
    while (value > 0U && i < (int)sizeof(tmp))
    {
        tmp[i++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (i > 0)
    {
        Gimbal_UartPutByte((uint8_t)tmp[--i]);
    }
}

/**
  * @brief  回显收到的原始报文，方便用串口助手确认协议
  */
static void Gimbal_WriteLogFrame(void)
{
    uint8_t i;

    Gimbal_WriteLogText("[gimbal] RX: ");
    for (i = 0; i < s_rx_len; i++)
    {
        Gimbal_UartPutByte(s_rx_buf[i]);
    }
    Gimbal_WriteLogText("\r\n");
}

/**
  * @brief  打印当前两个轴的角度
  */
static void Gimbal_WriteLog(const char *text)
{
    Gimbal_WriteLogText(text);
    Gimbal_WriteLogText(" A1=");
    Gimbal_WriteU32((uint32_t)s_axis_angle[0]);
    Gimbal_WriteLogText(" A2=");
    Gimbal_WriteU32((uint32_t)s_axis_angle[1]);
    Gimbal_WriteLogText("\r\n");
}
#endif /* GIMBAL_DEBUG_ECHO */

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  初始化云台：启动两路 PWM 并归中到正前方
  */
void Gimbal_Init(void)
{
    /* 两路 PWM 均已在 MX_TIM1_Init 中配置为 50Hz、PWM1 模式 */
    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);   /* 1 号舵机：水平 PE9  */
    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);   /* 2 号舵机：竖直 PE11 */

    Gimbal_ResetParser();

    /* 上电默认 90°，摄像头正前方 */
    Gimbal_SetAngle(GIMBAL_AXIS_PAN,  GIMBAL_ANGLE_MID);
    Gimbal_SetAngle(GIMBAL_AXIS_TILT, GIMBAL_ANGLE_MID);
}

/**
  * @brief  逐字节解析上位机指令
  * @param  byte: 本次收到的字节
  * @retval 1 = 解析出完整有效帧；0 = 尚未完成或帧非法
  * @note   支持 "g<轴>,<角度>a<轴>,<角度>r" 与 "s<轴>,<角度>r" 两种帧
  */
uint8_t Gimbal_ParseByte(uint8_t byte)
{
    uint8_t axis_index;

#if (GIMBAL_DEBUG_ECHO == 1)
    /* 记录原始报文，便于回显调试 */
    if (s_rx_len < GIMBAL_RX_BUF_SIZE)
    {
        s_rx_buf[s_rx_len++] = byte;
    }
#endif

    /* 任何字节到达都刷新超时计时 */
    s_last_byte_tick = HAL_GetTick();

    /* 半帧中途收到新的帧头：先丢弃残帧，再让该字节按帧头重新处理。
       否则丢包/干扰会把下一帧的帧头吃掉，导致连续丢帧。 */
    if (s_state != GIMBAL_ST_WAIT_HEAD && Gimbal_IsHeaderByte(byte))
    {
        Gimbal_ResetParser();
    }

    switch (s_state)
    {
        case GIMBAL_ST_WAIT_HEAD:
            if (Gimbal_IsHeaderByte(byte))
            {
                /* 'g' = 双轴云台帧，'s' = 单舵机帧 */
                s_is_gimbal_frame = ((byte == 'g' || byte == 'G')) ? 1U : 0U;
                s_state = GIMBAL_ST_AXIS;
            }
            else
            {
                /* 非帧头字符丢弃 */
#if (GIMBAL_DEBUG_ECHO == 1)
                s_rx_len = 0;
#endif
            }
            break;

        case GIMBAL_ST_AXIS:
            if (byte >= '0' && byte <= '9')
            {
                s_axis = (uint8_t)(byte - '0');
            }
            else if (byte == ',')
            {
                if (s_axis >= 1U && s_axis <= GIMBAL_AXIS_COUNT)
                {
                    s_angle = 0;
                    s_digit_count = 0;
                    s_state = GIMBAL_ST_ANGLE;
                }
                else
                {
                    /* 轴号非法：丢弃整帧 */
                    Gimbal_ResetParser();
                }
            }
            else
            {
                Gimbal_ResetParser();
            }
            break;

        case GIMBAL_ST_ANGLE:
            if (byte >= '0' && byte <= '9')
            {
                s_digit_count++;
                if (s_digit_count > 3U)
                {
                    Gimbal_ResetParser();   /* 角度不可能超过 3 位 */
                    break;
                }
                s_angle = (uint16_t)(s_angle * 10U + (uint16_t)(byte - '0'));
            }
            else if (byte == 'a' || byte == 'A')
            {
                /* 本轴角度结束，记录后接收下一个轴 */
                if (s_digit_count == 0U)
                {
                    Gimbal_ResetParser();   /* 缺少角度数字，整帧作废 */
                    break;
                }
                axis_index = (uint8_t)(s_axis - 1U);
                if (axis_index < GIMBAL_AXIS_COUNT)
                {
                    s_frame.angle[axis_index] = (uint8_t)GIMBAL_CLAMP_ANGLE((int)s_angle);
                    s_frame.target_mask |= (uint8_t)(1U << axis_index);
                    s_state = GIMBAL_ST_NEXT_AXIS;
                }
                else
                {
                    Gimbal_ResetParser();
                }
            }
            else if (byte == 'r' || byte == 'R')
            {
                /* 帧结束：记录本轴角度并提交 */
                if (s_digit_count == 0U)
                {
                    Gimbal_ResetParser();   /* 缺少角度数字，整帧作废 */
                    break;
                }
                axis_index = (uint8_t)(s_axis - 1U);
                if (axis_index < GIMBAL_AXIS_COUNT)
                {
                    s_frame.angle[axis_index] = (uint8_t)GIMBAL_CLAMP_ANGLE((int)s_angle);
                    s_frame.target_mask |= (uint8_t)(1U << axis_index);
                }
                return Gimbal_CommitFrame();
            }
            else
            {
                /* 含逗号等非法分隔符：整帧作废，不猜测用户意图 */
                Gimbal_ResetParser();
            }
            break;

        case GIMBAL_ST_NEXT_AXIS:
            if (byte >= '0' && byte <= '9')
            {
                s_axis = (uint8_t)(byte - '0');
            }
            else if (byte == ',')
            {
                if (s_axis >= 1U && s_axis <= GIMBAL_AXIS_COUNT)
                {
                    s_angle = 0;
                    s_digit_count = 0;
                    s_state = GIMBAL_ST_ANGLE;
                }
                else
                {
                    Gimbal_ResetParser();
                }
            }
            else if (byte == 'r' || byte == 'R')
            {
                /* 允许只给一个轴："g1,90r" */
                return Gimbal_CommitFrame();
            }
            else if (byte == 'a' || byte == 'A')
            {
                /* 连续的 'a'，忽略 */
            }
            else
            {
                Gimbal_ResetParser();
            }
            break;

        default:
            Gimbal_ResetParser();
            break;
    }

    return 0U;
}

/**
  * @brief  帧接收超时检查，需在主循环中周期调用
  * @note   若一帧只收到一半就断了（例如上位机被中断），
  *         状态机会一直等待。这里用空闲超时把残帧丢掉并复位。
  */
void Gimbal_PollTimeout(void)
{
    if (s_state != GIMBAL_ST_WAIT_HEAD)
    {
        if ((HAL_GetTick() - s_last_byte_tick) > GIMBAL_FRAME_TIMEOUT_MS)
        {
            Gimbal_ResetParser();
        }
    }
}

/**
  * @brief  直接把一帧指令应用到舵机
  * @param  frame: 待应用的帧
  * @note   只更新 target_mask 中置位的轴，未指定的轴保持原角度
  */
void Gimbal_ApplyFrame(const GimbalFrame_t *frame)
{
    uint8_t i;

    if (frame == NULL)
    {
        return;
    }

    for (i = 0; i < GIMBAL_AXIS_COUNT; i++)
    {
        if ((frame->target_mask & (uint8_t)(1U << i)) != 0U)
        {
            Gimbal_SetAngle((uint8_t)(i + 1U), (int)frame->angle[i]);
        }
    }
}

/**
  * @brief  设置指定轴的角度
  * @param  axis : 轴号，1 = 水平舵机，2 = 竖直舵机
  * @param  angle: 目标角度，超出 0-180 自动限幅
  */
void Gimbal_SetAngle(uint8_t axis, int angle)
{
    uint16_t pulse;
    uint8_t  index;

    if (axis < 1U || axis > GIMBAL_AXIS_COUNT)
    {
        return;
    }
    index = (uint8_t)(axis - 1U);

    /* 限幅，并记录当前姿态 */
    angle = GIMBAL_CLAMP_ANGLE(angle);
    s_axis_angle[index] = (uint8_t)angle;

    /* 线性映射角度 -> 脉冲宽度（1 计数 = 1us）
       0° -> 500us，180° -> 2500us，90° -> 1500us */
    pulse = (uint16_t)(GIMBAL_PULSE_MIN +
            ((int)(GIMBAL_PULSE_MAX - GIMBAL_PULSE_MIN) * angle) / GIMBAL_ANGLE_MAX);

    if (axis == GIMBAL_AXIS_PAN)
    {
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pulse);
    }
    else
    {
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, pulse);
    }
}

/**
  * @brief  查询指定轴当前角度
  * @param  axis: 轴号，1 = 水平舵机，2 = 竖直舵机
  * @retval 当前角度（0-180）；轴号非法时返回 255
  */
uint8_t Gimbal_GetAngle(uint8_t axis)
{
    if (axis < 1U || axis > GIMBAL_AXIS_COUNT)
    {
        return 255U;
    }
    return s_axis_angle[axis - 1U];
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
