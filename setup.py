from distutils.core import setup, Extension
import sysconfig

extra_compile_args = sysconfig.get_config_var("CFLAGS").split()
extra_compile_args += ["-Wall", "-Wextra", "-Wpedantic"]

mod = Extension(
    "rc522pi",
    sources=["pyinterface.c", "rc522c.c", "crc.c"],
    libraries=["pigpio"],
    extra_compile_args=extra_compile_args,
)

setup(
    name="rc522pi",
    version="1.0",
    description="RC522 SPI RFID module interface for Raspberry Pi based on pigpio. Supports NTAG21x.",
    ext_modules=[mod],
)
