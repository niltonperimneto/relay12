#!/usr/bin/env python3
"""Make a separate, unshippable source tree for upload-submit measurements."""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import check_d3d11on12_port as port  # noqa: E402


def make_variant(source, output):
    source, output = pathlib.Path(source).resolve(), pathlib.Path(output).resolve()
    if output == source or source in output.parents:
        raise ValueError("Research output must be outside the input tree")
    if output.exists():
        raise FileExistsError(output)
    device = source / "src/device.cpp"
    original = device.read_bytes()
    text = original.decode()
    errors = port.check_immediate_context_args(device, text)
    if errors:
        raise ValueError("; ".join(errors))
    body = port.IMM_CTX_ARGS.search(text)
    limit = port.UPLOAD_LIMIT.search(body.group(0))
    start = body.start() + limit.start("value")
    end = body.start() + limit.end("value")
    variant = text[:start] + "0u" + text[end:]
    shutil.copytree(source, output, symlinks=True,
                    ignore=shutil.ignore_patterns(".git"))
    (output / "src/device.cpp").write_bytes(variant.encode())
    manifest = {
        "researchOnly": True,
        "experiment": "measure/no-upload-limit",
        "productionDeviceSHA256": hashlib.sha256(original).hexdigest(),
        "variantDeviceSHA256": hashlib.sha256(variant.encode()).hexdigest(),
        "productionGateExpectedToFail": "non-zero upload submit limit",
        "sourceCommits": ["5a9eb523a5fc8db3a894deef6354f50e53a1c6da",
                          "9cf061bc3154c24b028d1dc17c1b3563c172642a"],
    }
    (output / "RESEARCH-ONLY.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prepared_source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    subprocess.run([sys.executable, str(ROOT / "scripts/check_d3d11on12_port.py"),
                    str(args.prepared_source)], check=True)
    make_variant(args.prepared_source, args.output)
    print("Research-only source created; production validation must reject this tree.")


if __name__ == "__main__":
    main()
