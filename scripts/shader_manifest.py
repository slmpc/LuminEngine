#!/usr/bin/env python3
"""从每个 shader companion JSON 生成 Slang 构建描述。

源码目录只保存面向作者的配置；CMake 只 include 本脚本生成的
``shader-targets.cmake``，因此 shader 入口、ABI 与构建命令不会再散落在
大型 CMake JSON 解析器中。
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Iterable


FEATURES = {"rayTracing", "nrd", "sharc"}
REQUIRED_ENTRY_FIELDS = ("name", "entry", "stage", "output", "reflection", "depfile")


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"无法读取 shader 配置 {path}: {error}") from error
    if not isinstance(value, dict):
        raise SystemExit(f"shader 配置必须是 JSON 对象: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def as_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "on", "true", "yes"}:
        return True
    if normalized in {"0", "off", "false", "no"}:
        return False
    raise argparse.ArgumentTypeError(f"无效的布尔值: {value}")


def feature_enabled(requirements: Iterable[str], features: dict[str, bool]) -> bool:
    unknown = set(requirements) - FEATURES
    if unknown:
        raise SystemExit(f"shader 使用了未知 feature: {', '.join(sorted(unknown))}")
    return all(features[requirement] for requirement in requirements)


def companion_paths(source_dir: Path) -> list[Path]:
    paths = []
    for path in sorted(source_dir.rglob("*.json")):
        if path.name == "shader-abi.json":
            continue
        payload = read_json(path)
        source = payload.get("source")
        inferred = path.with_suffix(".slang")
        if source or inferred.exists():
            paths.append(path)
    return paths


def resolve_source(source_dir: Path, source: str, config_path: Path) -> Path:
    source_path = (source_dir / source).resolve()
    if not source_path.is_file():
        raise SystemExit(f"{config_path}: shader source 不存在: {source}")
    return source_path


def collect(source_dir: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    abi_path = source_dir / "shader-abi.json"
    abi_payload = read_json(abi_path)
    if abi_payload.get("schemaVersion") != 1 or not isinstance(abi_payload.get("abi"), dict):
        raise SystemExit(f"{abi_path}: 需要 schemaVersion=1 且包含 abi 对象")

    compiler_defaults = {
        "target": "spirv",
        "profile": "spirv_1_5",
        "matrixLayout": "column-major",
        "warningsAsErrors": "all",
        "includeDirectories": ["include"],
    }
    compiler_defaults.update(abi_payload.get("compiler", {}))
    entries: list[dict[str, Any]] = []
    names: set[str] = set()
    outputs: set[str] = set()

    for config_path in companion_paths(source_dir):
        payload = read_json(config_path)
        source = payload.get("source") or config_path.relative_to(source_dir).with_suffix(".slang").as_posix()
        if not isinstance(source, str) or not source:
            raise SystemExit(f"{config_path}: source 必须是非空字符串")
        resolve_source(source_dir, source, config_path)

        compiler = dict(compiler_defaults)
        compiler.update(payload.get("compiler", {}))
        if compiler.get("warningsAsErrors") != "all":
            raise SystemExit(f"{config_path}: compiler.warningsAsErrors 必须为 all")

        raw_entries = payload.get("entries")
        if not isinstance(raw_entries, list) or not raw_entries:
            raise SystemExit(f"{config_path}: entries 必须是非空数组")
        for raw_entry in raw_entries:
            if not isinstance(raw_entry, dict):
                raise SystemExit(f"{config_path}: entries 中每一项必须是对象")
            missing = [field for field in REQUIRED_ENTRY_FIELDS if not raw_entry.get(field)]
            if missing:
                raise SystemExit(f"{config_path}: entry 缺少字段 {', '.join(missing)}")
            entry = dict(raw_entry)
            name = str(entry["name"])
            output = str(entry["output"])
            if name in names:
                raise SystemExit(f"shader entry 重复: {name}")
            if output in outputs:
                raise SystemExit(f"shader output 重复: {output}")
            names.add(name)
            outputs.add(output)
            entry["name"] = name
            entry["source"] = source
            entry.setdefault("requires", [])
            entry.setdefault("capabilities", [])
            entry.setdefault("bindings", [])
            entry.setdefault("abiStructs", [])
            entry["compiler"] = compiler
            entries.append(entry)

    manifest = {"schemaVersion": 1, "compiler": compiler_defaults, "abi": abi_payload["abi"], "shaders": entries}
    return manifest, entries


def cmake_quote(value: str | Path) -> str:
    text = str(value).replace("\\", "/").replace('"', '\\"')
    return f'"{text}"'


def cmake_list(values: Iterable[str | Path]) -> str:
    return " ".join(cmake_quote(value) for value in values)


def make_command(
    entry: dict[str, Any], manifest: dict[str, Any], source_dir: Path, output_dir: Path, slangc: Path
) -> list[str]:
    compiler = dict(manifest["compiler"])
    compiler.update(entry.get("compiler", {}))
    source = (source_dir / entry["source"]).resolve()
    output = (output_dir / entry["output"]).resolve()
    reflection = (output_dir / entry["reflection"]).resolve()
    depfile = (output_dir / entry["depfile"]).resolve()

    include_dirs: list[Path] = []
    for relative in [*compiler.get("includeDirectories", []), *entry.get("includeDirectories", [])]:
        directory = (source_dir / relative).resolve()
        if not directory.is_dir():
            raise SystemExit(f"{entry['name']}: shader include directory 不存在: {relative}")
        if directory not in include_dirs:
            include_dirs.append(directory)

    command = [
        cmake_quote(slangc),
        cmake_quote(source),
        "-target",
        cmake_quote(compiler["target"]),
        "-profile",
        cmake_quote(compiler["profile"]),
        "-warnings-as-errors",
        cmake_quote(compiler["warningsAsErrors"]),
    ]
    if compiler["target"] == "spirv":
        command.extend(["-fvk-use-entrypoint-name"])
    if compiler.get("matrixLayout"):
        command.extend([f"-matrix-layout-{compiler['matrixLayout']}"])
    capabilities = entry.get("capabilities", [])
    if capabilities:
        command.extend(["-capability", cmake_quote("+".join(capabilities))])
    for directory in include_dirs:
        command.extend(["-I", cmake_quote(directory)])
    for define in entry.get("defines", []):
        command.extend(["-D", cmake_quote(define)])
    command.extend(["-entry", cmake_quote(entry["entry"]), "-stage", cmake_quote(entry["stage"]),
                    "-reflection-json", cmake_quote(reflection), "-depfile", cmake_quote(depfile)])
    command.extend(cmake_quote(option) for option in entry.get("options", []))
    command.extend(["-o", cmake_quote(output)])
    return command


def generate_cmake(
    manifest: dict[str, Any],
    entries: list[dict[str, Any]],
    source_dir: Path,
    output_dir: Path,
    slangc: Path,
    features: dict[str, bool],
) -> str:
    manifest_path = (output_dir / "shader-manifest.json").resolve()
    lines = [
        "# Generated by scripts/shader_manifest.py; do not edit.",
        f"set(LUMIN_SHADER_MANIFEST {cmake_quote(manifest_path)})",
        "set(LUMIN_SHADER_OUTPUTS)",
        "set(LUMIN_SHADER_REFLECTIONS)",
    ]
    for entry in entries:
        if not feature_enabled(entry.get("requires", []), features):
            continue
        output = (output_dir / entry["output"]).resolve()
        reflection = (output_dir / entry["reflection"]).resolve()
        depfile = (output_dir / entry["depfile"]).resolve()
        source = (source_dir / entry["source"]).resolve()
        command = make_command(entry, manifest, source_dir, output_dir, slangc)
        output_parent = output.parent
        reflection_parent = reflection.parent
        depfile_parent = depfile.parent
        lines.extend([
            "add_custom_command(",
            f"    OUTPUT {cmake_list([output, reflection])}",
            f"    BYPRODUCTS {cmake_quote(depfile)}",
            "    COMMAND ${CMAKE_COMMAND} -E make_directory "
            f"{cmake_list([output_parent, reflection_parent, depfile_parent])}",
            "    COMMAND " + " ".join(command),
            f"    DEPENDS {cmake_list([source, manifest_path])}",
            f"    DEPFILE {cmake_quote(depfile)}",
            f"    COMMENT {cmake_quote('Compiling ' + entry['name'])}",
            "    VERBATIM",
            ")",
            f"list(APPEND LUMIN_SHADER_OUTPUTS {cmake_quote(output)})",
            f"list(APPEND LUMIN_SHADER_REFLECTIONS {cmake_quote(reflection)})",
        ])
    return "\n".join(lines) + "\n"


def migrate(legacy_path: Path, source_dir: Path) -> None:
    legacy = read_json(legacy_path)
    if legacy.get("schemaVersion") != 1 or not isinstance(legacy.get("shaders"), list):
        raise SystemExit(f"{legacy_path}: 不是 schemaVersion=1 的旧 shader manifest")
    write_json(source_dir / "shader-abi.json", {
        "schemaVersion": 1,
        "compiler": legacy["compiler"],
        "abi": legacy["abi"],
    })
    grouped: dict[str, list[dict[str, Any]]] = {}
    for raw_entry in legacy["shaders"]:
        entry = dict(raw_entry)
        source = entry.pop("source")
        grouped.setdefault(source, []).append(entry)
    for source, entries in grouped.items():
        payload = {
            "schemaVersion": 1,
            "source": source,
            "entries": entries,
        }
        write_json((source_dir / source).with_suffix(".json"), payload)


def command_generate(args: argparse.Namespace) -> None:
    source_dir = args.source_dir.resolve()
    output_dir = args.output_dir.resolve()
    slangc = args.slangc.resolve()
    manifest, entries = collect(source_dir)
    features = {"rayTracing": args.ray_tracing, "nrd": args.nrd, "sharc": args.sharc}
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output_dir / "shader-manifest.json"
    cmake_path = output_dir / "shader-targets.cmake"
    write_json(manifest_path, manifest)
    cmake_path.write_text(
        generate_cmake(manifest, entries, source_dir, output_dir, slangc, features),
        encoding="utf-8",
        newline="\n",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    migrate_parser = subparsers.add_parser("migrate", help="从旧总 manifest 生成 companion JSON")
    migrate_parser.add_argument("--legacy", type=Path, required=True)
    migrate_parser.add_argument("--source-dir", type=Path, required=True)
    migrate_parser.set_defaults(function=lambda args: migrate(args.legacy.resolve(), args.source_dir.resolve()))

    generate_parser = subparsers.add_parser("generate", help="生成构建目录中的 manifest 和 CMake targets")
    generate_parser.add_argument("--source-dir", type=Path, required=True)
    generate_parser.add_argument("--output-dir", type=Path, required=True)
    generate_parser.add_argument("--slangc", type=Path, required=True)
    generate_parser.add_argument("--ray-tracing", type=as_bool, default=True)
    generate_parser.add_argument("--nrd", type=as_bool, default=True)
    generate_parser.add_argument("--sharc", type=as_bool, default=True)
    generate_parser.set_defaults(function=command_generate)
    return parser


if __name__ == "__main__":
    parsed = build_parser().parse_args()
    parsed.function(parsed)
