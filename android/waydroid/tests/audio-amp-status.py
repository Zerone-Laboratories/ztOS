#!/usr/bin/env python3
"""Read MAX98512 status without changing amplifier configuration (phone, root)."""
import ctypes
import fcntl
import os


class Message(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("length", ctypes.c_uint16), ("buffer", ctypes.c_void_p)]


class Transfer(ctypes.Structure):
    _fields_ = [("messages", ctypes.POINTER(Message)), ("count", ctypes.c_uint32)]


fd = os.open('/dev/i2c-98', os.O_RDWR)
try:
    for address in (0x39, 0x3a):
        values = {}
        for register in (0x01, 0x02, 0x03, 0x35, 0x38, 0x39, 0x41,
                         0x4b, 0x4c, 0x86, 0x87, 0x88, 0x400):
            command = ctypes.create_string_buffer(bytes((register >> 8, register & 255)))
            result = ctypes.create_string_buffer(1)
            messages = (Message * 2)(
                Message(address, 0, 2, ctypes.addressof(command)),
                Message(address, 1, 1, ctypes.addressof(result)))
            transfer = Transfer(messages, 2)
            fcntl.ioctl(fd, 0x0707, transfer)
            values[f'{register:04x}'] = result.raw.hex()
        print(hex(address), values)
finally:
    os.close(fd)
