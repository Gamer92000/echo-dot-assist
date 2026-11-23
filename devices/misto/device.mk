# Echo 1 / EchoRoot, experimental. Use the misto target with a Linux cross compiler.
TARGET := arm-linux-musleabi
AUDIO := src/hassmic/audio_misto.c
WAKE := src/hassmic/wake_none.c
LIBS :=

RING := src/hassmic/ring_misto.c
