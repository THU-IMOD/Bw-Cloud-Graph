#!/usr/bin/env python3
"""Sequential, fully verified SMO comparisons. Raw artifacts stay outside the repo."""
import argparse
import hashlib
import json
import pathlib
import platform
import os
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="New output directory (stores and raw logs can be large)")
    parser.add_argument("--binary", type=pathlib.Path,
                        default=pathlib.Path("build/bw-test/multiscale_smo_bench"))
    parser.add_argument("--datasets", nargs="+", default=["rn_0.8", "yt_0.8", "rn_0.6", "yt_0.6"])
    parser.add_argument("--engines", nargs="+", choices=["legacy", "multiscale"],
                        default=["legacy", "multiscale"])
    parser.add_argument("--budgets", nargs="+", type=float, default=[256])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--writers", type=int, default=8)
    parser.add_argument("--smo-workers", type=int, default=1)
    parser.add_argument("--delta-pages", type=int, default=1)
    parser.add_argument("--delta-fraction", type=float, default=0.5)
    parser.add_argument("--layout", choices=["kaminpar-gorder", "louvain-gorder", "id-based", "id"], default="kaminpar-gorder")
    parser.add_argument("--layouts", nargs="+", choices=["kaminpar-gorder", "louvain-gorder", "id-based"])
    parser.add_argument("--bfs-datasets", nargs="+", default=[])
    parser.add_argument("--bfs-only", action="store_true")
    parser.add_argument("--bfs-mib", type=float, default=64)
    parser.add_argument("--bfs-rounds", type=int, default=10)
    parser.add_argument("--bfs-threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--chunks", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--continue-on-error", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    layouts = args.layouts or [args.layout]

    def digest(path):
        h = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                h.update(block)
        return h.hexdigest()

    metadata = {"host": platform.uname()._asdict(), "binary": str(binary),
                "binary_sha256": digest(binary), "args": vars(args).copy(), "inputs": {}}
    metadata["args"] = {k: str(v) if isinstance(v, pathlib.Path) else v
                        for k, v in metadata["args"].items()}
    for dataset in args.datasets:
        for suffix in ["graph", "delta"]:
            path = root / "data" / f"{dataset}.{suffix}"
            metadata["inputs"][str(path)] = digest(path)
        config = root / "config" / f"{dataset[:2]}.yaml"
        (output / config.name).write_bytes(config.read_bytes())
    for dataset in args.bfs_datasets:
        path = root / "data" / f"{dataset}.graph"
        metadata["inputs"][str(path)] = digest(path)
    bfs_binary = binary.parent / "multiscale_graph_bench"
    if args.bfs_datasets:
        metadata["bfs_binary"] = str(bfs_binary)
        metadata["bfs_binary_sha256"] = digest(bfs_binary)
    cache = binary.parent.parent / "CMakeCache.txt"
    if cache.exists():
        (output / "CMakeCache.txt").write_bytes(cache.read_bytes())
    metadata["compiler"] = subprocess.check_output(["c++", "--version"], text=True)
    metadata["submodules"] = subprocess.check_output(
        ["git", "submodule", "status"], cwd=root, text=True)
    metadata["git_head"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    (output / "source.diff").write_bytes(subprocess.check_output(["git", "diff"], cwd=root))
    # Include hashes of new untracked source files as well as tracked sources.
    files = subprocess.check_output(
        ["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=root).split(b"\0")
    metadata["sources"] = {name.decode(): digest(root / name.decode()) for name in set(files)
                           if name and (root / name.decode()).is_file()}
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2))
    records = []
    failed = False

    def run(name, command, kind):
        log = output / (name + ".log")
        with log.open("w") as stream:
            try:
                result = subprocess.run(command, cwd=root, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=args.timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                stream.write("\n[fail] benchmark timeout\n")
                code = 124
        text = log.read_text()
        records.append({"kind": kind, "command": command, "returncode": code, "output": text})
        (output / "results.json").write_text(json.dumps(records, indent=2))
        print(text.strip() or f"[fail] {name}: exit {code}", flush=True)
        return code

    if not args.bfs_only:
        for dataset in args.datasets:
            for budget in args.budgets:
                for repeat in range(1, args.repeats + 1):
                    for layout in layouts:
                        for engine in args.engines:
                            name = f"{dataset}-{layout}-{engine}-{budget:g}-r{repeat}"
                            command = [str(binary), str(root), dataset, engine, str(output / name),
                                       str(budget), str(args.writers), str(args.smo_workers),
                                       str(args.delta_pages), str(args.delta_fraction), str(args.chunks), layout]
                            code = run(name, command, "foreground-write")
                            failed |= code != 0
                            if code and not args.continue_on_error:
                                return 1

    for dataset in args.bfs_datasets:
        for layout in layouts:
            for engine in args.engines:
                name = f"bfs-{dataset}-{layout}-{engine}"
                config = (root / "config" / f"{dataset[:2]}.yaml").read_text()
                # These shipped dataset YAMLs have one occurrence of each setting.
                # Keep all other dataset options and save the effective YAML verbatim.
                values = {"name": dataset, "csr_prefix": str(output / name),
                          "delta_prefix": str(output / name), "meta_prefix": str(output / name),
                          "rebuild": True, "redo": True, "id_aware": layout in ["id", "id-based"],
                          "have_level": layout not in ["id", "id-based"]}
                for key, value in values.items():
                    config, count = re.subn(r"^  " + key + r":.*$",
                                           lambda _: "  " + key + ": " + json.dumps(value),
                                           config, flags=re.M)
                    if count != 1:
                        raise ValueError(f"Expected one YAML setting: {key}")
                config = re.sub(r"^  (engine|layout|full_giant_warmup):.*\n", "", config, flags=re.M)
                config = re.sub(r"^storage:.*$", lambda m: m[0] + "\n  engine: " + engine +
                                "\n  full_giant_warmup: true", config, flags=re.M)
                config = re.sub(r"^partition:.*$", lambda m: m[0] + "\n  layout: " + layout,
                                config, flags=re.M)
                config_path = output / (name + ".yaml")
                config_path.write_text(config)
                command = [str(bfs_binary), str(root), str(config_path), str(args.bfs_mib),
                           str(args.bfs_rounds), str(args.bfs_threads), "--build",
                           str(output / (name + ".samples.json"))]
                code = run(name, command, "warm-bfs")
                failed |= code != 0
                if code and not args.continue_on_error:
                    return 1
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
