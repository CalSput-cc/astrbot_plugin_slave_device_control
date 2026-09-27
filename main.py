from pydantic import Field
from pydantic.dataclasses import dataclass
from astrbot.api.event import filter, AstrMessageEvent, MessageEventResult
from astrbot.api.star import Context, Star, register
from astrbot.api import AstrBotConfig, logger
from openai import OpenAI
import serial
import json
import re


@register("astrbot_plugin_slave_device_control",
          "CalSput",
          "测试插件",
          "1.0.0")

class MyPlugin(Star):
    def __init__(self, context: Context, config: AstrBotConfig = None):
        super().__init__(context)
        self.config = config or {}
        logger.info("测试插件已初始化")
        
        #串口初始化
        try:
            self.ser = serial.Serial("/dev/ttyS2", 115200, timeout=1)
            logger.info("串口 /dev/ttyS2 打开成功")
        except Exception as e:
            logger.info(f"串口打开失败 {e}")
            self.ser = None
            
        #AI客户端
        self.deepseek_client = None
        self.deepseek_model = "deepseek-flash"

    async def initialize(self):
        """可选择实现异步的插件初始化方法，当实例化该插件类之后会自动调用该方法。
           创建deepseek客户端"""
        config = self.config or {}
        api_key = config.get("deepseek_api_key","")
        base_url = config.get("deepseek_base_url","")
        self.deepseek_model = config.get("deepseek_model","")
        if api_key:
            self.deepseek_client = OpenAI(api_key = api_key, base_url = base_url)
            logger.info(f"deepseek客户端初始化完成，模型为{self.deepseek_model}")
        else:
            logger.warning("未配置deepseek API key, 请配置后重载插件")
        
        

    # 注册指令的装饰器。指令名为 helloworld。注册成功后，发送 `/helloworld` 就会触发这个指令，并回复 `你好, {user_name}!`
    @filter.command("helloworld")
    async def helloworld(self, event: AstrMessageEvent):
        """这是一个 hello world 指令""" # 这是 handler 的描述，将会被解析方便用户了解插件内容。建议填写。
        user_name = event.get_sender_name()
        message_str = event.message_str # 用户发的纯文本消息字符串
        message_chain = event.get_messages() # 用户所发的消息的消息链 # from astrbot.api.message_components import *
        logger.info(message_chain)
        yield event.plain_result(f"Hello, {user_name}, 你发了 {message_str}!") # 发送一条纯文本消息

    @filter.command("测试")
    async def test(self, event: AstrMessageEvent):
        """收到 /测试 时会回复"""
        logger.info(f"已收到来自{event.get_sender_name()}的测试消息，这里是外设控制测试插件。")
        yield event.plain_result("插件加载成功，可以正常响应!")
    
    @filter.command("串口发送")
    async def gpiocontrol(self, event:AstrMessageEvent, message: str):
        """收到 /串口发送 message 时会通过核桃派串口发送message字符串内容，并返回消息"""
        user_name = event.get_sender_name()
        logger.info(f"已收到来自{event.get_sender_name()}串口命令，待发送内容是{message}")
        
        if self.ser is None or not self.ser.is_open:
            yield event.plain_result("串口未加载成功，请检查设备映射")
            logger.info("串口未加载成功，请检查设备映射")
            return
        
        try:
            self.ser.write(message.encode("utf-8"))
            yield event.plain_result(f"串口已发送消息{message}")
        except Exception as e:
            logger.info(f"串口发送失败:{e}")
            yield event.plain_result(f"串口发送失败:{e}")
            
    #自然语言控制舵机：上位机（运行astrbot，读取用户命令，deepseek（目前仅支持ds）解析出控制参数并通过串口发送 ——> 下位机接收参数并控制）
    @filter.event_message_type(filter.EventMessageType.ALL)     #监听所有消息
    async def on_all_message(self, event:AstrMessageEvent):
        """监听所有消息，并用ai解析"""
        message_str = event.message_str.strip()
        sender_name = event.get_sender_name()
        if not message_str or message_str.startswith("/"):
            return
        logger.info(f"收到来自{sender_name}的消息：{message_str}")
        
        #调用ai解析
        tar_angle = await self.ai_servo_angle(message_str)
        
        if tar_angle is None:
            return
        if tar_angle == -2:
            yield event.plain_result(f"角度超出范围：{tar_angle}")
            return
        if tar_angle == -3:
            yield event.plain_result("调用 AI解析 失败")
            return
            
        #串口发送        
        serial_flag = await self.send_servo_angle(tar_angle, device_id=1)
        
        if serial_flag:
            yield event.plain_result(f"已控制舵机转到 {tar_angle} 度")
        else:
            yield event.plain_result("串口发送失败，请检查设备")
    
    #ai解析
    async def ai_servo_angle(self, user_input: str):
        """调用ai解析的函数"""
        if self.deepseek_client is None:
            logger.warning("ai客户端未初始化，跳过解析")
            return None
        
        system_prompt = """
        你是一个舵机控制指令识别器。用户会用自然语言描述舵机转向。
        
        你的任务：
        1. 判断用户输入是否是控制舵机转向的命令。
        2. 如果是，提取出目标角度（0-180 度）。
        3. 如果不是舵机控制命令，返回 {"is_servo": false}。
        4. 请注意：用户也可能一段闲聊后跟随控制舵机的命令，注意识别！
        
        只返回一个 JSON，不要包含任何其他文字。
        
        格式：
        {"is_servo": true, "angle": <角度整数>}
        或
        {"is_servo": false}
        
        示例：
        - "把舵机转到90度" -> {"is_servo": true, "angle": 90}
        - "舵机向左转60度" -> {"is_servo": true, "angle": 60}
        - "帮我转向45度" -> {"is_servo": true, "angle": 45}
        - "今天天气不好，我的心情也不好，考四级没过！算了不说了，帮我转向45度" -> {"is_servo": true, "angle": 45}
        - "今天天气怎么样" -> {"is_servo": false}
        - "打开灯" -> {"is_servo": false}
        """
        
        try:
            ai_response = self.deepseek_client.chat.completions.create(
                model=self.deepseek_model,
                messages=[
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_input},
                ],
                stream=False,
                reasoning_effort="high",
                extra_body={"thinking": {"type": "enabled"}}           
            )
            content = ai_response.choices[0].message.content.strip()
            logger.info(f"AI返回；{content}")
            
            #可能的文本处理
            if content.startswith("```"):
                content = content.split("```")[1]
                if content.startswith("json"):
                    content = content[4:]
                content = content.strip()
            
            result = json.loads(content)
            if not result.get("is_servo"):
                return None
            angle = int(result.get("angle",-1))
            if not (0 <= angle <= 180):
                logger.warning(f"角度超出范围：{angle}")
                return -2
            return angle
        
        except json.JSONDecodeError as e:
            logger.error(f"JSON 解析失败: {e}, 原始内容: {content}")
            #正则
            if '"is_servo": false' in content or '"is_servo":false' in content:
                return None
            num = re.search(r"\d+",content)
            if num:
                angle = int(num.group())
                if not (0 <= angle <= 180):
                    logger.warning(f"正则_角度超出范围：{angle}")
                    return -2
                return angle
            return None  
            
        except Exception as e:
            logger.error(f"调用 DeepSeek 失败: {e}")
            return -3
        
    #串口发送角度
    async def send_servo_angle(self, angle: int, device_id: int) ->bool:
        """发送协议：s<device_id>,<tar_angle>r"""
        if self.ser is None or not self.ser.is_open:
            logger.error("串口未打开")
            return False

        command = f"s{device_id},{angle}r"
        try:
            self.ser.write(command.encode("ascii"))
            self.ser.flush()
            logger.info(f"串口已发送: {command}")
            return True
        except Exception as e:
            logger.error(f"串口发送失败: {e}")
            return False   
               
    async def terminate(self):
        """可选择实现异步的插件销毁方法，当插件被卸载/停用时会调用。"""
