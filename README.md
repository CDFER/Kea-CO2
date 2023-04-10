# Kea CO2 ✨

[![Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL&labelColor=5e2751&color=bc8c3d)](https://firstdonoharm.dev/version/3/0/cl-extr-ffd-media-mil-my-sv-tal.html)

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/d99afdea32c7452dbb50257498cd0df7)](https://www.codacy.com/gh/CDFER/OSAQS-Firmware/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=CDFER/OSAQS-Firmware&amp;utm_campaign=Badge_Grade)

You can now purchase these sensors through my website: https://www.keastudios.co.nz

A ESP32 based device to visualize co2 levels
    - Real-time graphs viewable through a web interface
    - External backup clock
    - Fail-safe file system to store data
    - Timezone support
    - Sync time through an unlocked Wi-Fi network Server (STA + AP Mode)
    - PCB Supports JTAG debugging (Through Breakout Connector)

![User interface](/images/ui1.png)

## Known limitations with current version
    - fixed 1min data recording interval
    - fixed 2mb max size data file (~60 days at 1min interval)

## Getting Started

If you test this code on a device, it would be helpful to add any issues you find to this repository.

### Prerequisites
The following libraries are used in Kea CO2:

    - espressif32 Arduino Framework
    - AsyncTCP-esphome @ 2.0.0 (LGPL (c) Hristo Gochkov @me-no-dev and others)
    - ESPAsyncWebServer-esphome @ 3.0.0 (LGPL (c) Hristo Gochkov @me-no-dev and others)
    - NeoPixelBus @ 2.7.3 (LGPL-3.0 license (c) Michael C. Miller and others)
    - ArduinoJson @ 6.20.1 (The MIT License (c) Benoit BLANCHON)
    - Apex Charts (JavaScript) (The MIT License)

### Installation
    1. Clone the repository.
    2. Install the required libraries (platformio does this automatically).
    3. Compile and upload the code to the ESP32 device.

### Usage

The Kea CO2 device lights up a strip of WS2812B addressable RGB LEDs to display a scale of the ambient CO2 level. CO2 data is from a Sensirion SCD40, and the LEDs are adjusted depending on the ambient light.

The device can download a spreadsheet of CO2, temperature, and humidity data. The data is stored in a fixed 2 MB file, which can store up to 60 days of data at a fixed 1-minute recording interval.

### Useful Extensions
    - Better C++ Syntax
    - Code Spell Checker
    - Live Server
    - Platformio IDE
    - Putty for Standalone Serial Logger

### Wiring
![Schematic](/images/Schematic.png)
| Name               | Designator         | Footprint                         | Quantity | Manufacturer Part        | Manufacturer               |
| ------------------ | ------------------ | --------------------------------- | -------- | ------------------------ | -------------------------- |
| CR1220             | BT1                | CR1220                            | 1        | BS-12-B2AA002            | MYOUNG(美阳)               |
| 4.7uF              | C1                 | C0402                             | 1        | CL05A475MP5NRNC          | SAMSUNG(三星)              |
| 10uF               | C2-C18             | C0402                             | 7        | CL05A106MQ5NUNC          | SAMSUNG(三星)              |
| 100nF              | C3-58              | C0402                             | 31       | CL05B104KO5NNNC          | SAMSUNG(三星)              |
| 100uF              | C7                 | CAP-SMD_L3.5-W2.8                 | 1        | TAJB107K006RNJ           | Kyocera AVX                |
| 22uF               | C10                | C0603                             | 1        | CL10A226MQ8NRNC          | SAMSUNG(三星)              |
| 22pF               | C21,C22,C23,C33    | C0402                             | 4        | 0402CG220J500NT          | FH(风华)                   |
| 47uF               | C25,C27,C60        | C0805                             | 3        | CL21A476MQYNNNE          | SAMSUNG(三星)              |
| 1nF                | C31,C61            | C0402                             | 2        | 0402B102K500NT           | FH(风华)                   |
| 1N4148             | D2                 | SOD-323_L1.8-W1.3-LS2.5-RD        | 1        | 1N4148WS                 | CJ(江苏长电/长晶)          |
| ZMM3V3             | D3                 | LL-34_L3.5-W1.5-RD                | 1        | ZMM3V3-M                 | ST(先科)                   |
| ZMM5V6             | D4                 | LL-34_L3.5-W1.5-RD                | 1        | ZMM5V6-M                 | ST(先科)                   |
| Ferrite Bead 800ma | FB1                | FB0805                            | 1        | GZ2012D101TF             | Sunlord(顺络)              |
| XC6220B331MR-G     | LDO1               | SOT-25-5_L3.0-W1.8-P0.95-LS3.0-BR | 1        | XC6220B331MR-G           | TOREX(特瑞仕)              |
| XC6206P332MR       | LDO2               | SOT-23-3_L2.9-W1.6-P1.90-LS2.8-BR | 1        | XC6206P332MR             | TOREX(特瑞仕)              |
| WS2812B            | LED1-32            | LED-SMD_XL-3535RGBC-WS2812B       | 22       | XL-3535RGBC-WS2812B      | XINGLIGHT(成兴光)          |
| RTC                | LED33              | LED0603-RD                        | 1        | KT-0603R                 |                            |
| 3V3_SCD4x          | LED36              | LED0603-RD                        | 1        | 19-217/GHC-YR1S2/3T      | EVERLIGHT(亿光)            |
| 3V3                | LED103             | LED0603-RD                        | 1        | 19-217/GHC-YR1S2/3T      | EVERLIGHT(亿光)            |
| RX                 | LED104             | LED0603-RD                        | 1        | 19-213/Y2C-CQ2R2L/3T(CY) | EVERLIGHT(亿光)            |
| TX                 | LED105             | LED0603-RD                        | 1        | 19-213/Y2C-CQ2R2L/3T(CY) | EVERLIGHT(亿光)            |
| IO2                | LED106             | LED0603-RD                        | 1        | KT-0603R                 |                            |
| SN74LV1T34DBVR     | LS1                | SOT-23-5_L3.0-W1.7-P0.95-LS2.8-BR | 1        | SN74LV1T34DBVR           | TI(德州仪器)               |
| 100kΩ              | R1,R5,R14,R15      | R0402                             | 4        | 0402WGF1003TCE           | UNI-ROYAL(厚声)            |
| 100Ω               | R2,R4              | R0402                             | 2        | 0402WGF1000TCE           | UNI-ROYAL(厚声)            |
| 10kΩ               | R3,R6,R7,R8,R9,R25 | R0402                             | 6        | 0402WGF1002TCE           | UNI-ROYAL(厚声)            |
| 200kΩ              | R10,R16            | R0402                             | 2        | 0402WGF2003TCE           | UNI-ROYAL(厚声)            |
| 200Ω               | R11                | R0402                             | 1        | 0402WGF2000TCE           | UNI-ROYAL(厚声)            |
| 1MΩ                | R13                | R0402                             | 1        | 0402WGF1004TCE           | UNI-ROYAL(厚声)            |
| 330Ω               | R17                | R0402                             | 1        | 0402WGF3300TCE           | UNI-ROYAL(厚声)            |
| 5.1kΩ              | R18,R19            | R0402                             | 2        | 0402WGF5101TCE           | UNI-ROYAL(厚声)            |
| 4.7kΩ              | R29,R30            | R0402                             | 2        | 0402WGF4701TCE           | UNI-ROYAL(厚声)            |
| RESET DATA         | SW1                | KEY-SMD_4P-L4.2-W3.2-P2.20-LS4.6  | 1        | SKRPACE010               | ALPSALPINE(阿尔卑斯阿尔派) |
| LTR303 Lux         | U4                 | SOT-363_L2.0-W1.3-P0.65-LS2.1-BL  | 1        | LTR-303ALS-01            | LITEON(光宝)               |
| PCF8563T RTC       | U6                 | SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL   | 1        | PCF8563T/5,518           | NXP(恩智浦)                |
| 918-418K2024S40004 | USBC1              | USB-C-SMD_918-418K2023S40013-1    | 1        | 918-418K2024S40004       | 精拓金                     |
| 32.768kHz          | X1                 | FC-135R_L3.2-W1.5                 | 1        | Q13FC1350000400          | EPSON(爱普生)              |




## ✌️ Other

Some of the foundational work was done as part of a project with Terrestrial Assemblages with the support of Govett-Brewster Art Gallery / Len Lye Centre

Thanks to everyone at @Espressif Systems for making a really awesome chip and porting it to arduino

Thanks to @LTRTNZ for putting up with my never ending shit and @vincentd123 for helping me

Made with love by Chris Dirks (@cd_fer) in Aotearoa New Zealand
