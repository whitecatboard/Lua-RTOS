# What's Lua RTOS?

Lua RTOS is a real-time operating system designed to run on embedded systems, with minimal requirements of FLASH and RAM memory. Currently Lua RTOS is available for ESP32, ESP8266 and PIC32MZ platforms, and can be easilly ported to other 32-bit platforms.

Lua RTOS is the main-core of the Whitecat ecosystem, that is being developed by a team of engineers, educators and living lab designers, designed for build Internet Of Things networks in an easy way.

Lua RTOS has a 3-layers design:

1. In the top layer there is a Lua 5.3.2 interpreter which offers to the programmer all resources provided by Lua 5.3.2 programming language, plus special modules for access the hardware (PIO, ADC, I2C, RTC, etc ...) and middleware services provided by Lua RTOS (LoRa WAN, MQTT, ...).

2. In the middle layer there is a Real-Time micro-kernel, powered by FreeRTOS. This is the responsible for that things happen in the expected time.

3. In the bottom layer there is a hardware abstraction layer, which talk directly with the platform hardware.

![](http://whitecatboard.org/git/luaos.png)

For porting Lua RTOS to other platforms is only necessary to write the code for the bottom layer, because the top and the middle layer are the same for all platforms.

# How is it programmed?

The Lua RTOS compatible boards can be programmed in two ways: using the Lua programming language directly, or using a block-based programming language that translates blocks to Lua. No matter if you use Lua or blocks, both forms of programming are made from the same programming environment. The programmer can decide, for example, to made a fast prototype using blocks, then change to Lua, and finally back to blocks.

![](http://whitecatboard.org/wp-content/uploads/2016/11/block-example.png)

![](http://whitecatboard.org/wp-content/uploads/2016/11/code-example.png)

In our [wiki] (https://github.com/whitecatboard/Lua-RTOS-ESP32/wiki) you have more information about this.

# How to build?

1. Install required drivers:

   Please note you need to download and install drivers for Win32 and MacOSX versions. The GNU/Linux version doesn't need any drivers, as usual ;)

   This drivers are required for connect to the Lua RTOS console through a serial port connection.

   You can download this drivers following one of this links:

   * [Linux 3.x.x](https://www.silabs.com/Support%20Documents/Software/Linux_3.x.x_VCP_Driver_Source.zip)
   * [Linux 2.6.x](https://www.silabs.com/Support%20Documents/Software/Linux_2.6.x_VCP_Driver_Source.zip)
   * [Mac OSX](https://www.silabs.com/Support%20Documents/Software/Mac_OSX_VCP_Driver.zip)
   * [Windows](https://www.silabs.com/Support%20Documents/Software/CP210x_Windows_Drivers.zip)

   You can get a full list of available drivers and versions [here](https://www.silabs.com/products/mcu/Pages/USBtoUARTBridgeVCPDrivers.aspx)

1. Install ESP32 toolchain for your desktop platform. Please, follow the instructions provided by ESPRESSIF:
   * [Windows] (https://github.com/espressif/esp-idf/blob/master/docs/windows-setup.rst)
   * [Mac OS] (https://github.com/espressif/esp-idf/blob/master/docs/macos-setup.rst)
   * [Linux] (https://github.com/espressif/esp-idf/blob/master/docs/linux-setup.rst)

1. Clone esp-idf repository from ESPRESSIF:

   ```lua
   git clone --recursive https://github.com/espressif/esp-idf.git
   ```

1. Clone Lua RTOS repository:

   ```lua
   git clone --recursive https://github.com/whitecatboard/Lua-RTOS-ESP32
   ```
   
1. Setup the build environment:
   
   Go to Lua-RTOS-ESP32 folder:
   
   ```lua
   cd Lua-RTOS-ESP32
   ```
   
   Edit the env file and change HOST_PLATFORM, PATH, IDF_PATH, LIBRARY_PATH, PKG_CONFIG_PATH, CPATH for fit to your installation locations.
   
   Now do:
   
   ```lua
   source ./env
   ```
   
1. Compile:

   First configure Lua RTOS options (located in Component config --> Lua RTOS):
 
   ```lua
   make menuconfig
   ```

   Build Lua RTOS, and flash to your ESP32 board:

   ```lua
   make flash
   ```

   Flash spiffs file system image to your ESP32 board:
   ```lua
   make flashfs
   ```
1. Connect to the console:

   You can connect to the Lua RTOS console using your favorite terminal emulator program, such as picocom, minicom, hyperterminal, putty, etc ... The connection parameters are:

   * speed: 115200 bauds
   * data bits: 8
   * stop bits: 1
   * parity: none
   * terminal emulation: VT100

   For example, if you use picocom:
   
   ```lua
   picocom --baud 115200 /dev/tty.SLAB_USBtoUART
   ```
   
   ```lua
      /\       /\
     /  \_____/  \
   /_____________\
   W H I T E C A T

   Lua RTOS beta 0.1 build 1479953238 Copyright (C) 2015 - 2016 whitecatboard.org
   cpu ESP32 at 240 Mhz
   spiffs0 start address at 0x180000, size 512 Kb
   spiffs0 mounted
   spi2 at pins sdi=012/sdo=013/sck=014/cs=015
   sd0 is at spi2, pin cs=015
   sd0 type II, size 1943552 kbytes, speed 15 Mhz
   sd0a partition type 0b, sector 227, size 1943438 kbytes
   fat init file system
   fat0 mounted
   redirecting console messages to file system ...

   Lua RTOS beta 0.1 powered by Lua 5.3.2

   Executing /system.lua ...
   Executing /autorun.lua ...

   / > 
   ```
