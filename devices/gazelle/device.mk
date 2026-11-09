TARGET := armv7a-linux-androideabi28
AUDIO := src/hassmic/audio_cube.c
WAKE := src/hassmic/wake_pryon.c
LIBS := libpryon.so libopus.so
CUBE := 1

# zlib comes from the NDK, not a vendor stub.
SYSLIBS := -lz
