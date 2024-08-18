# Kea CO2 ✨

[![Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL&labelColor=5e2751&color=bc8c3d)](https://firstdonoharm.dev/version/3/0/cl-extr-ffd-media-mil-my-sv-tal.html)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/d99afdea32c7452dbb50257498cd0df7)](https://www.codacy.com/gh/CDFER/OSAQS-Firmware/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=CDFER/OSAQS-Firmware&amp;utm_campaign=Badge_Grade)

Kea CO2 is an ESP32-based device designed to visualize CO2 levels. It offers real-time graphing capabilities through a web interface, an external backup clock, fail-safe data storage, timezone support, and time synchronization via an unlocked Wi-Fi network server (STA + AP mode). The PCB also supports JTAG debugging through a breakout connector.

![User interface](/images/ui1.png)

## Known Limitations with Current Version
- Fixed 1-minute data recording interval.
- Fixed 2 MB maximum size data file (~60 days at 1-minute interval).

## Getting Started

If you encounter any issues while testing this code on a device, please consider adding them to this repository.

### Prerequisites
The following libraries are used in Kea CO2:

- espressif32 Arduino Framework
- AsyncTCP-esphome (LGPL (c) Hristo Gochkov @me-no-dev and others)
- ESPAsyncWebServer-esphome (LGPL (c) Hristo Gochkov @me-no-dev and others)
- NeoPixelBus (LGPL-3.0 license (c) Michael C. Miller and others)
- ArduinoJson (The MIT License (c) Benoit blanchon)
- Apex Charts (JavaScript) (The MIT License)

### Installation
1. Clone the repository.
2. Install the required libraries (PlatformIO does this automatically).
3. Compile and upload the code to the ESP32 device.

### Usage

The Kea CO2 device illuminates a strip of WS2812B addressable RGB LEDs to display the ambient CO2 level scale. The CO2 data is obtained from a Sensirion SCD4X sensor, and the LEDs are adjusted based on the ambient light conditions.

The device is capable of downloading a spreadsheet containing CO2, temperature, and humidity data. The data is stored in a fixed 2 MB file, which can hold up to 60 days of data at a fixed 1-minute recording interval.

To Sync the Time through Wifi just setup a hotspot with the name time and password 12345678
On Power the device will connect to the network and sync the time (Green Pulse if successful).

### Useful Extensions
- Better C++ Syntax
- Code Spell Checker
- Live Server
- PlatformIO IDE
- PuTTY for Standalone Serial Logger

### Wiring
![Schematic](/images/Schematic.png)
| Name               | Designator    | Footprint | Quantity | Manufacturer Part   |
|--------------------|---------------|-----------|----------|---------------------|
| CR1220             | BT1           | CR1220    | 1        | BS-12-B2AA002       |
| 4.7uF              | C1            | C0402     | 1        | CL05A475MP5NRNC     |
| 10uF               | C2-C18        | C0402     | 7        | CL05A106MQ5NUNC     |
| 100nF              | C3-58         | C0402     | 31       | CL05B104KO5NNNC     |
| 100uF              | C7            | L3.5-W2.8 | 1        | TAJB107K006RNJ      |
| 22uF               | C10           | C0603     | 1        | CL10A226MQ8NRNC     |
| 22pF               | C21-23,C33    | C0402     | 4        | 0402CG220J500NT     |
| 47uF               | C25,C27,C60   | C0805     | 3        | CL21A476MQYNNNE     |
| 1nF                | C31,C61       | C0402     | 2        | 0402B102K500NT      |
| 1N148              | D2            | SOD-323   | 1        | 1N4148WS            |
| ZMM3V3             | D3            | LL-34     | 1        | ZMM3V3-M            |
| ZMM5V6             | D4            | LL-34D    | 1        | ZMM5V6-M            |
| Ferrite Bead 800ma | FB1           | FB0805    | 1        | GZ2012D101TF        |
| XC6220B331MR-G     | LDO1          | SOT-25-5  | 1        | XC6220B331MR-G      |
| XC6206P332MR       | LDO2          | SOT-23-3  | 1        | XC6206P332MR        |
| WS2812B            | LED1-32       | 3535RGB   | 22       | XL-3535RGBC-WS2812  |
| RTC                | LED33         | LED0603   | 1        | KT-0603R            |
| 3V3_SCD4x          | LED36         | LED0603   | 1        | 19-217/GHC-YR1S2/3T |
| 3V3                | LED103        | LED0603   | 1        | 19-217/GHC-YR1S2/3T |
| RX                 | LED104        | LED0603   | 1        | 19-213/Y2C-CQ2R2L   |
| TX                 | LED105        | LED0603   | 1        | 19-213/Y2C-CQ2R2L   |
| IO2                | LED106        | LED0603   | 1        | KT-0603R            |
| SN74LV1T34DBVR     | LS1           | SOT-23-5  | 1        | SN74LV1T34DBVR      |
| 100kΩ              | R1,R5,R14,R15 | R0402     | 4        | 0402WGF1003TCE      |
| 100Ω               | R2,R4         | R0402     | 2        | 0402WGF1000TCE      |
| 10kΩ               | R3,R6-R9,R25  | R0402     | 6        | 0402WGF1002TCE      |
| 200kΩ              | R10,R16       | R0402     | 2        | 0402WGF2003TCE      |
| 200Ω               | R11           | R0402     | 1        | 0402WGF2000TCE      |
| 1MΩ                | R13           | R0402     | 1        | 0402WGF1004TCE      |
| 330Ω               | R17           | R0402     | 1        | 0402WGF3300TCE      |
| 5.1kΩ              | R18,R19       | R0402     | 2        | 0402WGF5101TCE      |
| 4.7kΩ              | R29,R30       | R0402     | 2        | 0402WGF4701TCE      |
| LTR303 Lux         | U4            | SOT-363   | 1        | LTR-303ALS-01       |
| PCF8563T RTC       | U6            | SOIC-8    | 1        | PCF8563T/5,518      |
| 918-418K2024S40004 | USBC1         | USB-C-SMD | 1        | 918-418K2024S40004  |
| 32.768kHz          | X1            | FC-135R   | 1        | Q13FC1350000400     |

## ✌️ Other

Some of the foundational work was done as part of a project with Terrestrial Assemblages with the support of Govett-Brewster Art Gallery / Len Lye Centre.

Thanks to everyone at @Espressif Systems for making a really awesome chip and porting it to Arduino.

Thanks to @LTRTNZ for putting up with my never-ending shit and @vincentd123 for helping me.

Made with love by Chris Dirks (@cd_fer) in Aotearoa New Zealand.