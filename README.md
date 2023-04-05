# Kea CO2 ✨

[![Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL&labelColor=5e2751&color=bc8c3d)](https://firstdonoharm.dev/version/3/0/cl-extr-ffd-media-mil-my-sv-tal.html)

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/d99afdea32c7452dbb50257498cd0df7)](https://www.codacy.com/gh/CDFER/OSAQS-Firmware/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=CDFER/OSAQS-Firmware&amp;utm_campaign=Badge_Grade)

A ESP32 based device to visualize co2 levels

## 🖼️ User interface
![User interface](/images/ui1.png)

## Features
- download spreadsheet of co2, temp and humidity data
- realtime graphs viewable through web interface
- external backup clock
- fail safe file system to store data
- timezone support
- sync time through a unlocked wifi network Server (STA + AP Mode)
- PCB Supports Jtag debugging (Through Breakout Connector)

### Did it work?

If you test this code on a device it would be really helpful if you add issues you find on this repository


## Known limitations with current version

- fixed 1min data recording interval
- fixed 2mb max size data file (~60 days at 1min interval)


### Used Libraries

- espressif32 Arduino Framework
- AsyncTCP-esphome @ 2.0.0 (LGPL (c) Hristo Gochkov @me-no-dev and others)
- ESPAsyncWebServer-esphome @ 3.0.0 (LGPL (c) Hristo Gochkov @me-no-dev and others)
- NeoPixelBus @ 2.7.3 (LGPL-3.0 license (c) Michael C. Miller and others)
- ArduinoJson @ 6.20.1 (The MIT License (c) Benoit BLANCHON)
- Apex Charts (JavaScript) (The MIT License)

```c++
RAM:   [=         ]  14.3% (used 46844 bytes from 327680 bytes)
Flash: [========= ]  91.1% (used 932373 bytes from 1024000 bytes)
```

### Useful Extensions

- Better C++ Syntax
- Code Spell Checker
- Live Server
- Platformio IDE
- Putty for Standalone Serial Logger

### Wiring
![Schematic](/images/Schematic.png)
ESP32 Wroom Module
 - Lights up a strip of WS2812B Addressable RGB LEDs to display a scale of the ambient CO2 level
 - CO2 data is from a Sensirion SCD40
 - The LEDs are adjusted depending on the ambient Light


## ✌️ Other

Some of the foundational work was done as part of a project with Terrestrial Assemblages with the support of Govett-Brewster Art Gallery / Len Lye Centre

Thanks to @me-no-dev and everyone at @Espressif Systems for making a really awesome chip and porting it to arduino

Thanks to @LTRTNZ for putting up with my never ending shit and @vincentd123 for helping me with javascript and other things

Made with love by Chris Dirks (@cd_fer) in Aotearoa New Zealand