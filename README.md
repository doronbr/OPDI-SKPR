# OPDI-SKPR

This project is based on the ESP32-P4-WIFI6 board by waveshare (https://www.waveshare.com/wiki/ESP32-P4-WIFI6)featuring:

* LVGL based multi‑app UI (games, media, utilities) with PSRAM‑optimized memory usage
* MIPI-DSI display pipeline with tear avoidance & direct mode buffering
* MIPI-CSI camera (SC2336) with optional sensor flip controls & ISP pipeline
* Audio playback (MP3/WAV) with asynchronous startup tone/file logic
* SPIFFS storage for app assets (audio, game sounds, media)
* Customizable startup behavior (file playback or synthesized beep)
* Optional headless mode & selective peripheral bring-up for test fixtures
* Extensible partition table (large app region + SPIFFS storage)

## Getting Started


### Prerequisites

* An ESP32-P4-Function-EV-Board.
* A 7-inch 1024 x 600 LCD screen powered by the [EK79007](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-p4-function-ev-board/camera_display_datasheet/display_driver_chip_EK79007AD_datasheet.pdf) IC, accompanied by a 32-pin FPC connection [adapter board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-p4-function-ev-board/schematics/esp32-p4-function-ev-board-lcd-subboard-schematics.pdf) ([LCD Specifications](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-p4-function-ev-board/camera_display_datasheet/display_datasheet.pdf)).
* A MIPI-CSI camera powered by the SC2336 IC, accompanied by a 32-pin FPC connection [adapter board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-p4-function-ev-board/schematics/esp32-p4-function-ev-board-camera-subboard-schematics.pdf) ([Camera Specifications](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-p4-function-ev-board/camera_display_datasheet/camera_datasheet.pdf)).
* A USB-C cable for power supply and programming.
* Please refer to the following steps for the connection:
    * **Step 1**. According to the table below, connect the pins on the back of the screen adapter board to the corresponding pins on the development board.

        | Screen Adapter Board | ESP32-P4-Function-EV-Board |
        | -------------------- | -------------------------- |
        | 5V (any one)         | 5V (any one)               |
        | GND (any one)        | GND (any one)              |
        | PWM                  | GPIO26                     |
        | LCD_RST              | GPIO27                     |

    * **Step 2**. Connect the FPC of LCD through the `MIPI_DSI` interface.
    * **Step 3**. Connect the FPC of Camera through the `MIPI_CSI` interface.
    * **Step 4**. Use a USB-C cable to connect the `USB-UART` port to a PC (Used for power supply and viewing serial output).
    * **Step 5**. Turn on the power switch of the board.


### ESP-IDF Required

- This example supports ESP-IDF release/v5.4 and later branches. By default, it runs on ESP-IDF release/v5.4.
- Please follow the [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) to set up the development environment. **We highly recommend** you [Build Your First Project](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html#build-your-first-project) to get familiar with ESP-IDF and make sure the environment is set up correctly.

### Get the esp-dev-kits Repository

To start from the examples in esp-dev-kits, clone the repository to the local PC by running the following commands in the terminal:

```
git clone --recursive https://github.com/espressif/esp-dev-kits.git
```

### Configuration

Run ``idf.py menuconfig`` and explore board and project options:

```
menuconfig > Component config > Board Support Package
```

To use the SD card and enable the "Video Player" app, run ``idf.py menuconfig`` and then select ``Project  Configurations`` > ``Enable SD Card``.

**Video Playback Note**
To experience video playback, save MJPEG format videos on an SD card and insert the SD card into the SD card slot. **Currently, only MJPEG format videos are supported**. After inserting the SD card, the video playback app will automatically appear on the interface. The method for video format conversion is as follows:

* Install ffmpeg.
```
    sudo apt update
    sudo apt install ffmpeg
```
* Use ffmpeg to convert video.
```
   ffmpeg -i YOUR_INPUT_FILE_NAME.mp4 -vcodec mjpeg -q:v 2 -vf "scale=1024:600" -acodec copy YOUR_OUTPUT_FILE_NAME.mjpeg
```

## Project Configuration (menuconfig > Project  Configurations)

Below is a summary of the custom Kconfig options exposed under the renamed menu (``Project  Configurations``):

| Symbol | Type | Default | Description |
| ------ | ---- | ------- | ----------- |
| EXAMPLE_ENABLE_SD_CARD | bool | n | Mount SD card & enable video player assets from SD. |
| EXAMPLE_ENABLE_DISPLAY | bool | n | Initialize LCD + LVGL + UI stack. Disable for headless bench runs. |
| EXAMPLE_ENABLE_AUDIO_CODEC | bool | y | Initialize ES8311 audio codec for playback. |
| EXAMPLE_AUDIO_I2C_RETRY_COUNT | int (1-10) | 3 | Retry attempts when codec init I2C fails. |
| EXAMPLE_AUDIO_I2C_SCAN_BEFORE_INIT | bool | y | Probe address before creating codec instance. |
| EXAMPLE_AUDIO_I2C_DEBUG_SCAN | bool | y | Extra diagnostic scan/log for I2C address space. |
| EXAMPLE_FLASH_BAUD_921600 | bool | y | Advisory to flash at 921600 baud for faster cycles. |
| EXAMPLE_AUDIO_CODEC_GRACEFUL_FAIL | bool | y | Continue running if codec cannot be started. |
| EXAMPLE_HEADLESS_MODE | bool | n | Skip UI even if display code linked. Requires display disabled. |
| EXAMPLE_STARTUP_VOLUME | int (0-100) | 40 | Initial playback volume. |
| EXAMPLE_STARTUP_FILE_PATH | string | /spiffs/2048/weak.mp3 | SPIFFS path for startup audio clip. |
| EXAMPLE_STARTUP_BEEP_ENABLE | bool | y | Allow synthesized fallback tone. |
| EXAMPLE_STARTUP_BEEP_FREQ | int (100-4000) | 440 | Tone frequency (Hz). |
| EXAMPLE_STARTUP_BEEP_DURATION_MS | int (20-2000) | 120 | Tone duration (ms). |
| EXAMPLE_AUDIO_ASYNC_STARTUP | bool | y | Run startup audio in a background task. |
| EXAMPLE_ENABLE_CAM_SENSOR_PIC_VFLIP | bool | n | Vertical flip camera sensor output (added). |
| EXAMPLE_ENABLE_CAM_SENSOR_PIC_HFLIP | bool | n | Horizontal flip camera sensor output (added). |
| EXAMPLE_MIPI_CSI_SCCB_I2C_PORT | int | 0 | SCCB/I2C controller port for camera. |
| EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN | int | 8 | GPIO for camera SCL. |
| EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN | int | 7 | GPIO for camera SDA. |
| EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ | int | 100000 | I2C clock frequency (Hz). |
| EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN | int | -1 | Optional sensor reset GPIO (-1 = unused). |
| EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN | int | -1 | Optional sensor power-down GPIO (-1 = unused). |

These options correlate to logic in `components/apps/camera/app_video.c` and camera examples for conditional compilation and runtime configuration.

## Partition Layout

Current custom `partitions.csv`:

```
Name      Type   SubType  Offset    Size     Notes
--------- ------ -------- --------- -------- -------------------------------
nvs       data   nvs      0x11000   0x5000   Non-volatile storage
phy_init  data   phy      0x16000   0x1000   PHY calibration
factory   app    factory  0x20000   8M       Main application image
storage   data   spiffs   0x920000  4M       SPIFFS assets (audio, UI data)
```

## Build & Flash

Minimal quick start (replace PORT):

```
idf.py set-target esp32p4
idf.py -p PORT build
idf.py -p PORT flash monitor
```

Recommended higher baud if your USB bridge and cable are stable (when `EXAMPLE_FLASH_BAUD_921600=y`):

```
idf.py -p PORT -b 921600 flash
```

Exit monitor: `Ctrl-]`.

## SPIFFS Content

The `/spiffs` directory is packaged into the `storage` partition via `spiffs_create_partition_image`. Audio cue files under `spiffs/2048/` and music under `spiffs/music/` are referenced by startup logic and in-app features.

## Memory & Performance Notes

* Uses PSRAM @ 200 MHz with XIP text/rodata mapping for large LVGL & media code.
* Custom malloc hooks (via Brookesia memory options) can direct allocations to SPIRAM.
* LVGL caches (image, gradient, layer) sized for smoother multi-window transitions.

## Extending the Project

* Add a new app: create a subdirectory under `components/apps/` and register via its own `CMakeLists.txt` & component Kconfig.
* Add camera modes: introduce additional Kconfig enums for resolution/framerate and guard initialization code.
* Introduce OTA: adapt partition table to include `ota_0` / `ota_1` entries and enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.

    ...
    ```

## Contributing / Support

Please use the following feedback channels:

- For technical queries about OPDI-SKPR, open an issue in this repository.
- For underlying ESP-IDF platform questions, use the [esp32.com forum](https://esp32.com/).
- For Brookesia upstream specifics, refer to the upstream project’s issue tracker.

We will get back to you as soon as possible.
