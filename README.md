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


## ✌️ Other

Some of the foundational work was done as part of a project with Terrestrial Assemblages with the support of Govett-Brewster Art Gallery / Len Lye Centre

Thanks to everyone at @Espressif Systems for making a really awesome chip and porting it to arduino

Thanks to @LTRTNZ for putting up with my never ending shit and @vincentd123 for helping me

Made with love by Chris Dirks (@cd_fer) in Aotearoa New Zealand