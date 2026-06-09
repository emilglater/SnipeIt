"""
import fcntl
import struct
from pymodbus.client import ModbusSerialClient

# Linux RS485 ioctl from <linux/serial.h> — same one your hal_uart.c uses
TIOCSRS485 = 0x542F
SER_RS485_ENABLED     = (1 << 0)
SER_RS485_RTS_ON_SEND = (1 << 1)

client = ModbusSerialClient(port='/dev/ttyAMA2', baudrate=4800,
                            bytesize=8, parity='N', stopbits=1,
                            timeout=1)
client.connect()

# Enable kernel-managed RS485 mode on pymodbus's underlying fd.
# struct serial_rs485 is 8x uint32: flags, delay_before, delay_after, padding[5].
rs485_flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND
rs485_config = struct.pack('8I', rs485_flags, 0, 0, 0, 0, 0, 0, 0)
fcntl.ioctl(client.socket.fileno(), TIOCSRS485, rs485_config)

# Sanity check: read the current address. Should come back as 1.
print(client.read_holding_registers(address=0x07D0, count=1, slave=0x01))

# Change the address from 0x01 to 0x02.
client.write_register(address=0x07D0, value=0x02, slave=0x01)

client.close()
"""

# For reading back from the sensors:


import fcntl
import struct
from pymodbus.client import ModbusSerialClient

TIOCSRS485 = 0x542F
SER_RS485_ENABLED     = (1 << 0)
SER_RS485_RTS_ON_SEND = (1 << 1)

client = ModbusSerialClient(port='/dev/ttyAMA2', baudrate=4800,
                            bytesize=8, parity='N', stopbits=1,
                            timeout=1)
client.connect()

rs485_flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND
rs485_config = struct.pack('8I', rs485_flags, 0, 0, 0, 0, 0, 0, 0)
fcntl.ioctl(client.socket.fileno(), TIOCSRS485, rs485_config)

result = client.read_holding_registers(address=0x07D0, count=1, slave=0x01)
print("address register:", result.registers)   # expect [1]
# Verify the new address: read 0x07D0 from slave 0x02.
result = client.read_holding_registers(address=0x07D0, count=1, slave=0x02)
print("address register:", result.registers)   # expect [2]

# While we're here, read the live direction data too.
result = client.read_holding_registers(address=0x0000, count=1, slave=0x01)
print("[speed]:", result.registers)
result = client.read_holding_registers(address=0x0000, count=2, slave=0x02)
print("[gear, degrees]:", result.registers)    # e.g. [2, 90] for east

client.close()
