#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True


def load_generator(path: Path):
    spec = importlib.util.spec_from_file_location("lumin_shader_manifest", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 shader generator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ShaderManifestGeneratorTests(unittest.TestCase):
    generator = None

    def test_unchanged_generation_preserves_output_timestamps(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_dir = root / "shaders"
            output_dir = root / "generated"
            (source_dir / "include").mkdir(parents=True)
            (source_dir / "sample.slang").write_text(
                "[shader(\"compute\")] void computeMain() {}\n", encoding="utf-8"
            )
            (source_dir / "shader-abi.json").write_text(
                json.dumps({"schemaVersion": 1, "abi": {}}), encoding="utf-8"
            )
            (source_dir / "sample.json").write_text(
                json.dumps(
                    {
                        "schemaVersion": 1,
                        "source": "sample.slang",
                        "entries": [
                            {
                                "name": "sample.compute",
                                "entry": "computeMain",
                                "stage": "compute",
                                "output": "sample.comp.spv",
                                "reflection": "reflection/sample.comp.json",
                                "depfile": "deps/sample.comp.d",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            arguments = argparse.Namespace(
                source_dir=source_dir,
                output_dir=output_dir,
                slangc=Path(sys.executable),
                ray_tracing=False,
                nrd=False,
                sharc=False,
            )
            self.generator.command_generate(arguments)

            generated_paths = [output_dir / "shader-manifest.json", output_dir / "shader-targets.cmake"]
            old_timestamp = 1_000_000_000
            for path in generated_paths:
                os.utime(path, (old_timestamp, old_timestamp))

            self.generator.command_generate(arguments)

            for path in generated_paths:
                self.assertEqual(path.stat().st_mtime_ns, old_timestamp * 1_000_000_000)

            targets = generated_paths[1].read_text(encoding="utf-8")
            dependency_line = next(line for line in targets.splitlines() if line.strip().startswith("DEPENDS"))
            self.assertIn("sample.slang", dependency_line)
            self.assertNotIn("shader-manifest.json", dependency_line)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("generator", type=Path)
    parsed, unittest_arguments = parser.parse_known_args()
    ShaderManifestGeneratorTests.generator = load_generator(parsed.generator.resolve())
    unittest.main(argv=[sys.argv[0], *unittest_arguments])
