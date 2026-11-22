import os
from pathlib import Path
import shutil
import struct
import subprocess

import numpy as np
import pytest


def read_weights(path):
    data = path.read_bytes()
    layer_count = struct.unpack_from("<I", data, 16)[0]
    cursor = 20
    descriptors = []
    for _ in range(layer_count):
        descriptors.append(struct.unpack_from("<III", data, cursor))
        cursor += 12
    mean = np.frombuffer(data, dtype="<f4", count=11, offset=cursor).copy()
    cursor += 11 * 4
    standard_deviation = np.frombuffer(data, dtype="<f4", count=11, offset=cursor).copy()
    cursor += 11 * 4
    layers = []
    for input_width, output_width, _activation in descriptors:
        weights = np.frombuffer(
            data, dtype="<f4", count=input_width * output_width, offset=cursor
        ).copy().reshape(output_width, input_width)
        cursor += input_width * output_width * 4
        biases = np.frombuffer(data, dtype="<f4", count=output_width, offset=cursor).copy()
        cursor += output_width * 4
        layers.append((weights, biases))
    return mean, standard_deviation, layers


def test_cpp_mlp_matches_pytorch_on_1000_fixed_vectors(tmp_path):
    torch = pytest.importorskip("torch")
    from torch import nn

    parity_binary = os.environ.get("TICKBOOK_NN_PARITY_BIN")
    if not parity_binary:
        parity_binary = shutil.which("nn_parity")
    if not parity_binary:
        pytest.skip("build nn_parity and set TICKBOOK_NN_PARITY_BIN to run C++ parity")

    root = Path(__file__).resolve().parents[2]
    weights_path = root / "tests" / "data" / "tiny_weights.bin"
    mean, standard_deviation, layers = read_weights(weights_path)
    rng = np.random.default_rng(20260403)
    raw = rng.integers(-1000, 1001, size=(1000, 11), dtype=np.int64)
    raw[:, 1:3] *= 700
    raw[:, 3] *= 900
    raw[:, 4:9] *= 100_000
    raw[:, 9:11] *= 950
    input_path = tmp_path / "vectors.bin"
    input_path.write_bytes(raw.astype("<i8", copy=False).tobytes(order="C"))

    result = subprocess.run(
        [parity_binary, str(weights_path), str(input_path)],
        check=True,
        capture_output=True,
    )
    cpp_output = np.frombuffer(result.stdout, dtype="<f4").reshape(1000, 3)

    class TorchReference(nn.Module):
        def __init__(self):
            super().__init__()
            self.layers = nn.ModuleList(
                [
                    nn.Linear(11, layers[0][0].shape[0]),
                    nn.Linear(layers[1][0].shape[1], layers[1][0].shape[0]),
                    nn.Linear(layers[2][0].shape[1], layers[2][0].shape[0]),
                ]
            )
            for layer, (weights, biases) in zip(self.layers, layers, strict=True):
                layer.weight.data.copy_(torch.from_numpy(weights))
                layer.bias.data.copy_(torch.from_numpy(biases))

        def forward(self, values):
            normalized = (values - torch.from_numpy(mean)) / torch.from_numpy(standard_deviation)
            normalized = torch.relu(self.layers[0](normalized))
            normalized = torch.relu(self.layers[1](normalized))
            return self.layers[2](normalized)

    torch.set_num_threads(1)
    model = TorchReference().eval()
    with torch.no_grad():
        torch_output = model(torch.from_numpy(raw.astype(np.float32))).numpy()
    np.testing.assert_allclose(cpp_output, torch_output, rtol=1e-5, atol=1e-5)
