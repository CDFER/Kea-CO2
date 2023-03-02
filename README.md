# Kea CO2 ✨

[![Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL&labelColor=5e2751&color=bc8c3d)](https://firstdonoharm.dev/version/3/0/cl-extr-ffd-media-mil-my-sv-tal.html)

[![Codacy Badge](https://app.codacy.com/project/badge/Grade/d99afdea32c7452dbb50257498cd0df7)](https://www.codacy.com/gh/CDFER/OSAQS-Firmware/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=CDFER/OSAQS-Firmware&amp;utm_campaign=Badge_Grade)

A ESP32 Firmware written in Arduino C++ and works with the SCD4x CO2 Sensors from Sensirion

## 🖼️ User interface
![User interface](/images/ui1.png)

### Compile yourself using PlatformIO

- Make sure Git client is installed on your system. https://github.com/git-guides/install-git
- Download and install Visual Studio Code by Microsoft. https://code.visualstudio.com/download
- Open Visual Studio Code and go to the Extensions manager (the icon with the stacked blocks in the left bar)
- Search for platform.io and install the PlatformIO extension
- Download the source code by running a git clone (git gui can be found in your right click menu)
- In VS Code Go to File -> Open Folder and open that root folder (the one that contains platformio.ini, NOT the src folder)
- Upload to the esp32 using the right arrow button in the bottom left corner of vs code (it takes awhile for the first compile)


### Did it work?

If you test this code on a device it would be really helpful if you add issues you find on this repository


## Known limitations with current version

- fixed 1min data recording interval
- fixed 1mb csv data file (~30 days at 1min interval)


### Future Dev Options to look into (Help or suggestions are appreciated):

- Support external i2c RTC (uRTCLib?)
- Sync Time from ntp Server (STA + AP Mode)
- Move to non blocking internal i2c tasks (both SCD4X and VEML7700 libraries are blocking)
- Larger CSV data Log File Support 
- file.size() test speed with large (1mb+) files
- Buffer small webserver file reads from SPIFFS
- Switch to LittleFS 
- Switch to uPlot (https://github.com/leeoniya/uPlot) from Apex Charts (Smaller Faster)

- Jtag Support
- Better Programming and debug support (Through Panel Edge Connector?)
- Add Error LED
- Add Physical Clear Data / Reset Button
- Switch to XL-3535RGBC-WS2812B
- Stored Serial Number (append to AP SSID)
- Consider other i2c Lux Sensors
- More LEDs!
- Power Panic Save Data

### Used Libraries

- espressif32 Arduino Framework
- AsyncTCP @ 1.1.1+sha.ca8ac5f (LGPL (c) Hristo Gochkov @me-no-dev and others)
- ESP Async WebServer @ 1.2.3+sha.f71e3d4 (LGPL (c) Hristo Gochkov @me-no-dev and others)
- NeoPixelBus @ 2.7.3 (LGPL-3.0 license (c) Michael C. Miller and others)
- Sensirion I2C SCD4x @ 0.3.1+sha.923aa94 (BSD 3-Clause License)
- DFRobot_VEML7700 @ 1.0.0 (The MIT License)
- ArduinoJson @ 6.20.1 (The MIT License (c) Benoit BLANCHON)
- Apex Charts (JavaScript) (The MIT License)

RAM:   [=         ]  13.6% (used 44716 bytes from 327680 bytes)
Flash: [========= ]  85.9% (used 879213 bytes from 1024000 bytes)

### Useful Extensions

- Better C++ Syntax
- Code Spell Checker
- Live Server
- Platformio IDE
- Putty for Standalone Serial Logger

### Wiring
![Schematic](/images/schematic.png)
ESP32 Wroom Module
 - Lights up a strip of WS2812B Addressable RGB LEDs to display a scale of the ambient CO2 level
 - CO2 data is from a Sensirion SCD40
 - The LEDs are adjusted depending on the ambient Light data from a VEML7700
 - There is also a webserver which displays graphs of CO2, Humidity, Temperature and Lux while also providing a csv download for that data

 - i2c (IO21 -> SDA, IO22 ->SCL) -> SCD40 & VEML7700 (3.3V Power and Data)
 - IO2 (3.3V) -> SN74LVC2T45 Level Shifter (5v) -> (9x) WS2812B mini (5v Power and Data)
 - USB C power (5v Rail) -> XC6220B331MR-G -> 3.3V Rail

## ✌️ Other

Some of the foundational work was done as part of a project with Terrestrial Assemblages with the support of Govett-Brewster Art Gallery / Len Lye Centre

Thanks to @me-no-dev and everyone at @Espressif Systems for making a really awesome chip and porting it to arduino

Thanks to @LTRTNZ for putting up with my never ending shit and @vincentd123 for helping me with javascript and other things

Made with love by Chris Dirks (@cd_fer) in Aotearoa New Zealand