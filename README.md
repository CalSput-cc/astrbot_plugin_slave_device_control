# astrbot_plugin_slave_device_control

一个基于 AstrBot 的外设控制插件，用于通过 QQ 消息远程控制开发板（这里以核桃派2B为例），并对外设、GPIO（这里以串口通信为例）进行操作。

这里以二自由度云台和摄像头为例，赋予聊天机器人睁眼看世界的能力。

> [!NOTE]
> 本项目为学习性质的开源项目，主要用于学习 AstrBot 插件开发与嵌入式 GPIO 控制的结合。该插件对于不同的开发板环境并不具有普适性（这里以核桃派为例），部分GPIO操作可能由于引脚编号不同、设备节点不同、代码使用的库不同等原因而失效，如有在其他设备移植需求请自行查找并映射设备节点，修改相应库和代码。
> 
> 请注意：目前AI解析只支持deepseek的API
>
> AI代码使用声明：下位机固件部分由AI辅助生成。 
> 
> [AstrBot](https://github.com/AstrBotDevs/AstrBot) is an agentic assistant for both personal and group conversations. It can be deployed across dozens of mainstream instant messaging platforms, including QQ, Telegram, Feishu, DingTalk, Slack, LINE, Discord, Matrix, etc. In addition, it provides a reliable and extensible conversational AI infrastructure for individuals, developers, and teams. Whether you need a personal AI companion, an intelligent customer support agent, an automation assistant, or an enterprise knowledge base, AstrBot enables you to quickly build AI applications directly within your existing messaging workflows.

# 功能

- 通过 QQ 指令控制开发板（这里以核桃派2B为例）GPIO
- 支持拍照功能
- 支持通过串口给下位机发送指令，控制舵机。（这里以一个二自由度云台加摄像头为例）
- 可扩展其他功能

# 硬件要求
- 核桃派 WalnutPi 2B
- usb摄像头
- 180°舵机
- 单片机（如stm32）作为下位机
- 如有需求可增加其他模块

# 环境需求
- AstrBot 部署在 Docker 容器中
- 容器需映射 GPIO 设备节点
- 容器映射串口节点
- 容器映射摄像头节点
- 如有需求，可自行映射其他设备节点

## Docker 设备映射示例

```yaml:
services:
  astrbot:
    devices:
      - "/dev/ttyS2:/dev/ttyS2"      # 串口
      - "/dev/video0:/dev/video0"    # USB 摄像头
      - "/dev/gpiochip0:/dev/gpiochip0"  # GPIO 控制器
    group_add:
      - "dialout"
```
注意：不同开发板的设备节点名不同（如核桃派没有 /dev/gpiomem），请用 ls /dev/ 确认实际节点。

# 使用示例

| QQ 消息 | 效果 |
|---|---|
| `/拍照` | 拍一张照片并返回 |
| `/串口发送 hello` | 通过串口发送 `hello` |
| `/测试` | 测试插件是否加载成功 |
| `把舵机转到90度` | AI 解析角度，串口发送 `s1,90r` |
| `往左看看` | 云台转向左侧并拍照返回 |

## 演示



# 相关链接
- https://github.com/AstrBotDevs/AstrBot
- https://docs.astrbot.app/dev/star/plugin-new.html
- https://wiki.walnutpi.com/

# License
- AGPL-3.0
