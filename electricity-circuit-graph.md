AI Calculator Machine 完整接线图
====================================================

[电池部分]
----------------------------------------------------
1500mAh 电池 正极/红线  ─┐
                         ├──> 充放电模块 BAT+
1200mAh 电池 正极/红线  ─┘

1500mAh 电池 负极/黑/蓝线 ─┐
                           ├──> 充放电模块 BAT-
1200mAh 电池 负极/黑/蓝线 ─┘


[电源输出部分]
----------------------------------------------------
充放电模块 5V OUT+  ──> 自锁开关 一脚
自锁开关 另一脚     ──> ESP32-S3 CAM 的 5V pin

充放电模块 GND / OUT- ──> ESP32-S3 CAM 的 GND pin


[Camera 部分]
----------------------------------------------------
OV5640 DVP AF 摄像头
──> 直接插入 ESP32-S3 CAM 的 camera FPC 插座

不要另外飞线。


[OLED SSD1305 2.23"：使用 I2C 模式]
----------------------------------------------------
OLED VDD        ──> ESP32 3V3
OLED VSS / GND  ──> ESP32 GND

OLED BS1        ──> ESP32 3V3
OLED BS2        ──> ESP32 GND

OLED SCLK / D0  ──> ESP32 GPIO2    // I2C SCL
OLED SDIN / D1  ──> ESP32 GPIO1    // I2C SDA
OLED D2         ──> ESP32 GPIO1    // 和 D1 接一起

OLED RES / RST  ──> ESP32 GPIO47

OLED CS         ──> ESP32 GND
OLED D/C / RS / SA0 ──> ESP32 GND  // I2C 地址通常 0x3C

OLED WR         ──> 不接
OLED RD         ──> 不接
OLED D3         ──> 不接
OLED D4         ──> 不接
OLED D5         ──> 不接
OLED D6         ──> 不接
OLED D7         ──> 不接
OLED NC         ──> 不接


[按钮部分：6 个轻触开关]
----------------------------------------------------
全部按钮统一规则：

按钮一脚 ──> GND
按钮另一脚 ──> 对应 ESP32 GPIO

具体如下：

上一页按钮：
一脚 ──> GND
另一脚 ──> ESP32 GPIO14

下一页按钮：
一脚 ──> GND
另一脚 ──> ESP32 GPIO21

上一题按钮：
一脚 ──> GND
另一脚 ──> ESP32 GPIO38

下一题按钮：
一脚 ──> GND
另一脚 ──> ESP32 GPIO39

拍照按钮：
一脚 ──> GND
另一脚 ──> ESP32 GPIO40

确定按钮：
一脚 ──> GND
另一脚 ──> ESP32 GPIO41

备用/切换页面按钮，如果你之后加：
一脚 ──> GND
另一脚 ──> ESP32 GPIO42


[GND 总线]
----------------------------------------------------
以下全部 GND 必须接在一起：

充放电模块 GND
ESP32 GND
OLED GND / VSS
所有按钮其中一脚

也就是：

充放电模块 GND
        │
        ├── ESP32 GND
        ├── OLED VSS / GND
        ├── 上一页按钮 GND脚
        ├── 下一页按钮 GND脚
        ├── 上一题按钮 GND脚
        ├── 下一题按钮 GND脚
        ├── 拍照按钮 GND脚
        └── 确定按钮 GND脚


[不要使用的 ESP32 GPIO]
----------------------------------------------------
以下 GPIO 留给 camera，不要接按钮/OLED：

GPIO4
GPIO5
GPIO6
GPIO7
GPIO8
GPIO9
GPIO10
GPIO11
GPIO12
GPIO13
GPIO15
GPIO16
GPIO17
GPIO18

尽量也不要用：

GPIO0    // BOOT
GPIO45   // 启动相关
GPIO46   // 启动相关
GPIO19   // USB D+
GPIO20   // USB D-


[代码里对应设置]
----------------------------------------------------
OLED I2C：

SDA = GPIO1
SCL = GPIO2

Wire.begin(1, 2);


按钮全部用 INPUT_PULLUP：

GPIO14 = 上一页
GPIO21 = 下一页
GPIO38 = 上一题
GPIO39 = 下一题
GPIO40 = 拍照
GPIO41 = 确定
GPIO42 = 备用

按下时读取结果 = LOW


[最终总图]
----------------------------------------------------

1500mAh Battery +
                   ┐
1200mAh Battery +  ├── BAT+   [充放电模块]
                   │
1500mAh Battery -  ├── BAT-
1200mAh Battery -  ┘

[充放电模块]
5V OUT+ ──> 自锁开关 ──> ESP32 5V
GND     ─────────────────> ESP32 GND

[ESP32-S3 CAM]
Camera FPC <────────────── OV5640 DVP AF

GPIO2  ───────────────────> OLED SCLK / D0
GPIO1  ───────────────────> OLED SDIN / D1
GPIO1  ───────────────────> OLED D2
GPIO47 ───────────────────> OLED RES / RST
3V3    ───────────────────> OLED VDD + OLED BS1
GND    ───────────────────> OLED VSS + OLED BS2 + OLED CS + OLED D/C/SA0

GPIO14 ───────────────────> 上一页按钮
GPIO21 ───────────────────> 下一页按钮
GPIO38 ───────────────────> 上一题按钮
GPIO39 ───────────────────> 下一题按钮
GPIO40 ───────────────────> 拍照按钮
GPIO41 ───────────────────> 确定按钮
GPIO42 ───────────────────> 备用按钮

所有按钮另一脚 ───────────> GND