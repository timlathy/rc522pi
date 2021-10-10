# Connecting RC522 to Raspberry Pi M3B+:
# 3.3V -> pin 17
# RST -> pin 22 (GPIO 25)
# GND -> pin 20
# IRQ -> pin 18 (GPIO 24)
# MISO -> pin 21
# MOSI -> pin 19
# SCK -> pin 23
# SDA -> pin 24 (CE0)

# Don't forget to enable SPI:
# 1. Launch sudo raspi-config
# 2. Go to Interface Options -> P4 SPI
# 3. Enable the interface
# 4. Close raspi-config and reboot

from rc522pi import RC522, RC522Error, RC522TagError
from time import sleep

PASSWORD = b"\xAB\x06\x05\xFF"
PACK = b"\xB0\xBA"

try:
    rc522 = RC522(spi_baud_rate=1_000_000, antenna_gain=4, rst_pin=25)
    print(f"Device version: {hex(rc522.dev_version)}")
    while not rc522.ntag_try_select():
        pass
    print(f"Detected {rc522.tag_kind} with NFCID {rc522.tag_nfcid}")

    # Section 8.8.1 of the NTAG21x datasheet recommends to "diversify the password and the password acknowledge"
    # using the tag ID. For the sake of example, let's XOR the bytes of the pwd/pack and the ID:
    tag_pwd = bytes(rc522.tag_nfcid[i] ^ p for i, p in enumerate(PASSWORD))
    tag_pack = bytes(rc522.tag_nfcid[i] ^ p for i, p in enumerate(PACK))

    try:
        pack = rc522.ntag_authenticate(tag_pwd)
        if pack != tag_pack:
            print(f"Auth failed: tag's PACK does not match expected value")
    except RC522TagError as e:
        # NTAG21x datasheet, section 8.4: "the command interpreter returns to the idle state on receipt of an unexpected command"
        # So if the authentication fails, the tag returns to the idle state and needs to be selected again.
        while not rc522.ntag_try_select():
            pass
        # Let's assume that the tag is not protected yet (the configuration pages are available for writing).
        # It's recommended to always password-protect at least the configuration pages
        # so that someone else doesn't do it later, locking your tag from you.
        # In this example, all pages starting from 8 will be read and write protected
        # Other possible mode is 'w': reading does not require auth while writing does
        rc522.ntag_protect(pwd=tag_pwd, pack=tag_pack, start_page=8, mode='rw')
        # Now you're still able to read pages 0...7 without authentication, but request page 8 or higher and you get a NAK =)

    # Set the contents of page 6 to 0xCA 0xFE 0xB0 0xBA
    rc522.ntag_write(6, b"\xCA\xFE\xB0\xBA")

    # Read the contents of pages 4, 5, 6, 7
    start_page = 4
    data = list(rc522.ntag_read(start_page))
    for p in range(0, 4):
        print(f"Page {start_page + p}: {[hex(b) for b in data[p*4:(p+1)*4]]}")

except RC522Error as e:
    # Error hierarchy: RC522Error <- RC522TagError
    # This is a catch-all for tag errors (recoverable) as well as device/IO errors (may be unrecoverable).
    # A possible recovery strategy is to try to create a new instance of RC522 and repeat your actions.
    # The instance constructor performs a hard reset of the device, which may help.
    #
    # Note that some errors could require a full reboot of RPi. For instance, you might see
    # a pigpio error "init mbox zaps failed". This means there's not enough free GPU (DMA) memory.
    # It may happen when the finalizer for RC522, which performs pigpio cleanup, doesn't get run
    # (e.g. due to the Python process getting SIGKILL'ed).
    print(f"Error: {e}")
