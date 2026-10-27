#!/usr/bin/env python3
"""Deliberately crash a 64 MiB process to verify bounded coredump handling."""
import os
memory = bytearray(64 * 1024 * 1024)
for offset in range(0, len(memory), 4096):
    memory[offset] = 1
os.abort()
