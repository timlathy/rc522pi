# rc522pi

A Python interface for using the RC522 SPI RFID module with NTAG21x tags on Raspberry Pi.

Note: for a general-purpose RPi RC522 library, refer to [pi-rc522](https://github.com/ondryaso/pi-rc522). In comparison, this project:

* Focuses on **NTAG21x** and does not support other tags (not even the MIFARE Classic series).
* Provides high-level wrappers for the main NTAG21x functionality: reading/writing data, authenticating, configuring password protection.
* Has a polling-based interface, does not use the `IRQ` pin.
* Replaces return codes with exceptions, which not only make the code quite a bit cleaner, but also allow errors to have informative messages.
* Works with (and was actually developed for) MFRC522 clones with the `0x12` version code. Unlike the original chips, [they don't support soft reset](https://github.com/miguelbalboa/rfid/wiki/Chinese_RFID-RC522), so the code performs a hard reset on initialization instead.
* Uses a _pigpio_-based C implementation, which means it's slightly faster, but _requires root to run_.

## Installation

The project is not available on PyPI at the moment. You can install it system-wide on RPi with the following commands:

```sh
git clone https://github.com/timlathy/rc522pi
cd rc522pi
python3 setup.py install
```

## Usage

Check out [the usage example](examples/usage.py) to see the module in action.
