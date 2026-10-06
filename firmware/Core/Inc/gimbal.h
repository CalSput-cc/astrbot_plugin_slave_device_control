/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gimbal.h
  * @brief   二自由度云台控制模块（与 AstrBot 插件 astrbot_plugin_slave_device_control 对接）
  *
  * 上位机（AstrBot 插件）串口协议：
  *   云台帧 : g<轴号>,<角度>a<轴号>,<角度>r
  *            例: "g1,180a2,90r" -> 1 号舵机 180°，2 号舵机 90°
  *   单舵机 : s<轴号>,<角度>r
  *            例: "s1,90r"        -> 1 号舵机 90°
  *
  * 轴号与硬件对应关系：
  *   1 号舵机（水平/方位轴，控制 x-y 平面） -> TIM1_CH1 -> PE9
  *   2 号舵机（竖直/俯仰轴，控制 z 平面）   -> TIM1_CH2 -> PE11
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __GIMBAL_H__
#define __GIMBAL_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* 云台轴号定义 -------------------------------------------------------------*/
#define GIMBAL_AXIS_PAN    1U   /* 水平舵机（1 号），TIM1_CH1 / PE9  */
#define GIMBAL_AXIS_TILT   2U   /* 竖直舵机（2 号），TIM1_CH2 / PE11 */
#define GIMBAL_AXIS_COUNT  2U   /* 云台自由度数量                     */

/* 舵机角度定义 -------------------------------------------------------------*/
#define GIMBAL_ANGLE_MIN   0
#define GIMBAL_ANGLE_MAX   180
#define GIMBAL_ANGLE_MID   90   /* 上电默认位置：正前方 */

/* PWM 脉冲宽度定义（TIM1 计数时钟 1MHz，1 计数 = 1us） ---------------------*/
#define GIMBAL_PULSE_MIN   500  /* 0.5ms ->   0° */
#define GIMBAL_PULSE_MID   1500 /* 1.5ms ->  90° */
#define GIMBAL_PULSE_MAX   2500 /* 2.5ms -> 180° */

/* 接收超时：帧间隔超过该时间（ms）则丢弃半帧，防止残帧卡死解析器 ----------*/
#define GIMBAL_FRAME_TIMEOUT_MS  100U

/* 角度限幅辅助宏 -----------------------------------------------------------*/
#define GIMBAL_CLAMP_ANGLE(a)  (((a) < GIMBAL_ANGLE_MIN) ? GIMBAL_ANGLE_MIN : \
                                (((a) > GIMBAL_ANGLE_MAX) ? GIMBAL_ANGLE_MAX : (a)))

/* Exported types ------------------------------------------------------------*/

/**
  * @brief  接收解析状态机
  */
typedef enum
{
    GIMBAL_ST_WAIT_HEAD = 0,   /* 等待帧头 'g' / 's'              */
    GIMBAL_ST_AXIS,            /* 接收轴号                        */
    GIMBAL_ST_ANGLE,           /* 接收角度                        */
    GIMBAL_ST_NEXT_AXIS        /* 云台帧中，等待 'a' 引入下一个轴 */
} GimbalState_t;

/**
  * @brief  一帧云台指令
  */
typedef struct
{
    uint8_t target_mask;                 /* 位掩码，bit0 = 轴1，bit1 = 轴2 */
    uint8_t angle[GIMBAL_AXIS_COUNT];    /* 各轴目标角度（已限幅到 0-180） */
} GimbalFrame_t;

/* Exported functions prototypes ---------------------------------------------*/

/* 初始化：设置 PWM 通道并归中到正前方 */
void     Gimbal_Init(void);

/* 逐字节送入解析状态机；返回 1 表示解析出一个完整有效帧并已应用到舵机 */
uint8_t  Gimbal_ParseByte(uint8_t byte);

/* 复位解析状态机（串口出错、需要丢弃残帧时调用） */
void     Gimbal_ResetParser(void);

/* 帧接收超时检查，需在主循环中周期调用 */
void     Gimbal_PollTimeout(void);

/* 直接把一帧指令应用到舵机（可由中断上下文调用） */
void     Gimbal_ApplyFrame(const GimbalFrame_t *frame);

/* 直接设置某个轴的角度，内部自动限幅 */
void     Gimbal_SetAngle(uint8_t axis, int angle);

/* 查询某个轴当前角度，轴号非法时返回 255 */
uint8_t  Gimbal_GetAngle(uint8_t axis);

#ifdef __cplusplus
}
#endif

#endif /* __GIMBAL_H__ */
