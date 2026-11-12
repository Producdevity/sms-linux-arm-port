#!/usr/bin/env python3
"""Export the pinned decompilation and apply the compatibility series.

Series entries named sms-pc-port/<file> come from the pinned sms-pc-port
submodule's decomp-patches (MIT); the others are this port's own."""
import argparse
import filecmp
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKER = ".sunshine-port.json"


_LITERAL = re.compile(
    r'(?P<comment>//[^\n]*(?:\n|$)|/\*[\s\S]*?\*/)'
    r'|(?P<raw>(?P<raw_prefix>u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})'
    r'\([\s\S]*?\)(?P=delimiter)")'
    r'|(?P<quoted>(?P<prefix>u8|u|U|L)?(?P<quote>["\'])'
    r'(?:\\[\s\S]|(?!(?P=quote))[^\\\r\n])*(?P=quote))'
)


def narrow_literal_bytes(source):
    """Encode non-ASCII narrow literals as Shift-JIS bytes, as MWCC -multibyte did."""
    def replace(match):
        token = match.group()
        if match.group('comment') or token.isascii():
            return token
        if match.group('raw'):
            if match.group('raw_prefix'):
                return token
            delimiter = match.group('delimiter')
            content = token[len(delimiter) + 3:-(len(delimiter) + 2)]
            return '"' + ''.join(f'\\{byte:03o}' for byte in content.encode('cp932')) + '"'
        if match.group('prefix'):
            return token
        return ''.join(char if ord(char) < 128 else
                       ''.join(f'\\{byte:03o}' for byte in char.encode('cp932'))
                       for char in token)
    return _LITERAL.sub(replace, source)


def prepare_execution_strings(root):
    for path in root.rglob('*'):
        if path.is_file():
            if path.suffix not in ('.c', '.cpp', '.h', '.hpp', '.inl', '.pch', '.mch'):
                continue
            original = path.read_text(encoding='utf-8')
            converted = narrow_literal_bytes(original)
            if converted != original:
                path.write_text(converted, encoding='utf-8')


def git(cwd, *args, **kwargs):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)


def tree_digest(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if relative == MARKER or (path.is_dir() and not path.is_symlink()):
            continue
        digest.update(relative.encode() + b"\0")
        if path.is_symlink():
            digest.update(b"link\0" + os.readlink(path).encode())
        else:
            digest.update(str(path.stat().st_mode & 0o777).encode() + b"\0")
            digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def prepare(source, output, lock_path, series_path):
    if output.is_symlink():
        raise ValueError("Generated output must not be a symlink")
    source, output = source.resolve(), output.resolve()
    if output == source or source in output.parents or output in source.parents:
        raise ValueError("Generated output must be separate from upstream")
    revision = json.loads(lock_path.read_text())["revision"]
    actual = git(source, "rev-parse", "HEAD").stdout.decode().strip()
    if actual != revision:
        raise ValueError(f"Upstream HEAD {actual} does not match pinned {revision}")
    if git(source, "status", "--porcelain", "--untracked-files=normal").stdout:
        raise ValueError("Upstream must be pristine")
    patches = []
    names = series_path.read_text().splitlines() if series_path.exists() else []
    for name in names:
        name = name.strip()
        if not name or name.startswith("#"):
            continue
        if name.startswith("sms-pc-port/"):
            base = (ROOT / "third_party/sms-pc-port/decomp-patches").resolve()
            patch = (base / name[len("sms-pc-port/"):]).resolve()
        else:
            base = series_path.parent.resolve()
            patch = (base / name).resolve()
        if patch.parent != base or patch.suffix != ".patch" or not patch.is_file():
            raise ValueError(f"Invalid patch entry: {name}")
        if patch in patches:
            raise ValueError(f"Duplicate patch entry: {name}")
        patches.append(patch)
    identity = {
        "revision": revision,
        "patches": [{"name": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in patches],
        "preparer": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    marker = output / MARKER
    if output.exists():
        if not marker.is_file():
            raise ValueError(f"Refusing to replace unmanaged output: {output}")
        previous = json.loads(marker.read_text())
        if not isinstance(previous, dict) or "inputs" not in previous or "tree_sha256" not in previous:
            raise ValueError(f"Refusing to replace unrecognized output: {output}")
        if tree_digest(output) != previous["tree_sha256"]:
            raise ValueError("Generated source was modified; preserve and inspect that build tree")
        if previous.get("inputs") == identity:
            print(f"Super Mario Sunshine {revision[:12]}: source already prepared")
            return
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    try:
        subprocess.run(["git", "clone", "--quiet", "--shared", "--no-checkout",
                        str(source), str(stage)], check=True, capture_output=True)
        git(stage, "checkout", "--quiet", "--detach", revision)
        for patch in patches:
            # sms-pc-port applies its series with patch(1), which tolerates offsets and fuzz.
            subprocess.run(["patch", "-p1", "--batch", "--forward", "--quiet", "-d", str(stage), "-i", str(patch)],
                           check=True, capture_output=True)
        # patch(1) keeps a .orig copy of each file it fuzzed.
        for leftover in stage.rglob("*.orig"):
            if leftover.is_file() and stage in leftover.parents:
                leftover.unlink()
        prepare_execution_strings(stage)
        if output.exists():
            for path in stage.rglob("*"):
                previous = output / path.relative_to(stage)
                if (path.is_symlink() or previous.is_symlink() or
                        not path.is_file() or not previous.is_file()):
                    continue
                if (path.stat().st_mode == previous.stat().st_mode and
                        filecmp.cmp(path, previous, shallow=False)):
                    os.utime(path, ns=(path.stat().st_atime_ns, previous.stat().st_mtime_ns))
        shutil.rmtree(stage / ".git")
        (stage / MARKER).write_text(json.dumps(
            {"inputs": identity, "tree_sha256": tree_digest(stage)}, indent=2) + "\n")
        backup = output.with_name(output.name + ".previous")
        if backup.exists():
            raise ValueError(f"Previous output requires inspection: {backup}")
        if output.exists():
            output.rename(backup)
        try:
            stage.rename(output)
        except BaseException:
            if backup.exists():
                backup.rename(output)
            raise
        if backup.exists():
            shutil.rmtree(backup)
        print(f"Super Mario Sunshine {revision[:12]}: prepared with {len(patches)} patches")
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "third_party/sms")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lock", type=Path, default=ROOT / "third_party/sms.lock.json")
    parser.add_argument("--series", type=Path, default=ROOT / "patches/sms/series")
    args = parser.parse_args()
    try:
        prepare(args.source, args.output, args.lock, args.series)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", b"")
        print(f"Super Mario Sunshine preparation failed: {error}", file=sys.stderr)
        if detail:
            print(detail.decode(errors="replace"), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
