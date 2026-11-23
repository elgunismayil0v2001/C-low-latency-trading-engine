import struct
import zlib

import numpy as np
import pytest

from export_weights import encode_weights
from export_weights import HIDDEN_WIDTH


def tiny_layers(output_width=3, hidden_width=HIDDEN_WIDTH):
    return [
        (np.zeros((hidden_width, 11), np.float32), np.zeros(hidden_width, np.float32), 1),
        (np.zeros((hidden_width, hidden_width), np.float32), np.zeros(hidden_width, np.float32), 1),
        (np.zeros((output_width, hidden_width), np.float32), np.zeros(output_width, np.float32), 0),
    ]


def test_weights_export_is_deterministic_and_crc_covers_payload():
    mean = np.arange(11, dtype=np.float32)
    standard_deviation = np.ones(11, dtype=np.float32)
    first = encode_weights(mean, standard_deviation, tiny_layers())
    second = encode_weights(mean, standard_deviation, tiny_layers())
    assert first == second
    assert first[:8] == b"TICKNN01"
    assert struct.unpack_from("<III", first, 8) == (1, 0x01020304, 3)
    assert struct.unpack_from("<I", first, len(first) - 4)[0] == zlib.crc32(first[:-4])


def test_weights_export_accepts_width_32():
    blob = encode_weights(np.zeros(11), np.ones(11), tiny_layers(hidden_width=32))
    assert struct.unpack_from("<III", blob, 20) == (11, 32, 1)
    assert struct.unpack_from("<III", blob, 32) == (32, 32, 1)


def test_weights_export_rejects_bad_architecture_and_normalization():
    with pytest.raises(ValueError, match="standard deviations"):
        encode_weights(np.zeros(11), np.zeros(11), tiny_layers())
    with pytest.raises(ValueError, match="exactly three"):
        encode_weights(np.zeros(11), np.ones(11), tiny_layers()[:2])
    invalid = tiny_layers()
    invalid[1] = (np.zeros((HIDDEN_WIDTH - 1, HIDDEN_WIDTH), np.float32),
                  np.zeros(HIDDEN_WIDTH - 1, np.float32), 1)
    with pytest.raises(ValueError, match="width 16"):
        encode_weights(np.zeros(11), np.ones(11), invalid)

    too_wide = tiny_layers(hidden_width=33)
    with pytest.raises(ValueError, match="between 1 and 32"):
        encode_weights(np.zeros(11), np.ones(11), too_wide)
