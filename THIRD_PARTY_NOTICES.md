# Third-Party Notices

This project is distributed under the GNU GPLv3. It depends on third-party
software that remains under its own licenses.

## Included dependencies

### Arduino-ESP32

- Upstream: <https://github.com/espressif/arduino-esp32>
- Package: `framework-arduinoespressif32`
- License: `LGPL-2.1-or-later`
- Local metadata in the build environment: `.platformio/packages/framework-arduinoespressif32/package.json`

### Arduino-ESP32 precompiled libraries

- Upstream: <https://github.com/espressif/esp32-arduino-lib-builder>
- Package: `framework-arduinoespressif32-libs`
- License: `LGPL-2.1-or-later`
- Local metadata in the build environment: `.platformio/packages/framework-arduinoespressif32-libs/package.json`

### ESP-IDF components used underneath Arduino-ESP32

- Upstream: <https://github.com/espressif/esp-idf>
- Version in this build environment: `v5.5.4`
- Espressif states that ESP-IDF is composed mostly of `Apache-2.0` licensed
  components, plus some third-party components under other compatible
  licenses.
- Official reference: <https://www.espressif.com/zh-hans/products/sdks/esp-idf>

## Notes

- This file is provided for notice and attribution convenience. It is not a
  substitute for the original license texts of the third-party projects.
- If you redistribute binaries or source releases, keep this notice file and
  the project `LICENSE` together.
- If you vendor third-party source into this repository in the future, include
  the original upstream license texts alongside that vendored code.
