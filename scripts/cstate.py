import ctypes
import signal

f = open("/dev/cpu_dma_latency", "wb")
f.write(ctypes.c_int32(1))
f.flush()

while True:
  signal.pause()
