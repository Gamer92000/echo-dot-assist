"""Where the stock model and test audio are, for the tools of this folder (run from the repository root).

PRYON_MODEL: the Alexa set's ONNX model of the unpacked firmware (default: devices/donut's firmware/donut/rootfs/...).
PRYON_AUDIO: raw 16 kHz s16le test audio (default: testdata/alexa_espeak.raw).
"""
import os
from pathlib import Path

ROOT = Path(os.environ.get('ECHO_DOT_ASSIST', Path(__file__).resolve().parents[2]))
DEFAULT_SOURCE = Path(os.environ.get('PRYON_MODEL', ROOT/'firmware/donut/rootfs/system/local/models/keyword/en-US/ALEXA/runtime_stateless_standard.streaming.fixed.int16.onnx'))
DEFAULT_AUDIO = Path(os.environ.get('PRYON_AUDIO', ROOT/'testdata/alexa_espeak.raw'))
