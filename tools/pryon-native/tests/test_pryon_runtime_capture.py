"""Small offline checks for capture decoding and measured numerical rules."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import numpy as np
from pryon_debug_capture import Remote,harness_main
from pryon_runtime_analysis import quantize_rows,pack_weight_pairs,lfbe


class FragmentedSocket:
    def __init__(self,data):self.data=bytearray(data)
    def sendall(self,data):pass
    def recv(self,size):
        data=bytes(self.data[:1]);del self.data[:1];return data


def test_fragmented_rsp_and_run_length():
    remote=Remote.__new__(Remote)
    payload=b'0* '+b'}'+bytes([ord('#')^32])
    remote.sock=FragmentedSocket(b'+$'+payload+b'#'+f'{sum(payload)&255:02x}'.encode())
    assert remote.command('g')=='0000#'


def test_quantization_and_packing():
    q,s,z=quantize_rows(np.array([[-1.,0.,126.],[0.,63.5,127.]],np.float32))
    np.testing.assert_array_equal(q,[[0,1,127],[0,64,127]])
    np.testing.assert_array_equal(s,[1,1])
    np.testing.assert_array_equal(z,[1,0])
    try:  # half-way zero-point tie reaches 128; runtime saturation is unmeasured
        quantize_rows(np.array([[-127.,0.,127.]],np.float32));raise AssertionError('expected rejection')
    except ValueError:pass
    np.testing.assert_array_equal(pack_weight_pairs(np.array([[1,2],[3,4],[5,6]])),[1,3,2,4,5,6])


def test_frontend_silence():
    np.testing.assert_array_equal(lfbe(np.zeros(560,dtype=np.int16)),np.zeros((2,64)))


def test_harness_rejects_unknown_binary():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p=Path(d)/'harness';p.write_bytes(b'synthetic, unapproved harness')
        try:
            harness_main(p)
        except RuntimeError:
            return
        raise AssertionError('unapproved harness was accepted')


if __name__=='__main__':
    test_harness_rejects_unknown_binary()
    test_fragmented_rsp_and_run_length()
    test_quantization_and_packing()
    test_frontend_silence()
    print('4 capture/analysis checks passed')
