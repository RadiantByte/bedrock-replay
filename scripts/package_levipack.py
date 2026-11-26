import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

VALUE_PATTERN = re.compile(r'^\s*inline\s+constexpr\s+std::string_view\s+(Name|Author|Description|Version)\s*=\s*"((?:\\.|[^"\\])*)";\s*$')
REQUIRED_VALUES = ("Name", "Author", "Description", "Version")
RESOURCE_PACK_ARCHIVE_ROOT = "resources/minecraft_resource_packs"
BEHAVIOR_PACK_ARCHIVE_ROOT = "resources/minecraft_behavior_packs"


def parse_version(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = VALUE_PATTERN.match(line)
        if match:
            values[match.group(1)] = bytes(match.group(2), "utf-8").decode("unicode_escape")
    missing = [name for name in REQUIRED_VALUES if not values.get(name)]
    if missing:
        raise ValueError("Missing version metadata: " + ", ".join(missing))
    return values


def build_manifest(values: dict[str, str], library_name: str, bundled_roots: list[str]) -> dict[str, object]:
    return {
        "type": "preload-native",
        "name": values["Name"],
        "author": values["Author"],
        "description": values["Description"],
        "version": values["Version"],
        "entry": library_name,
        "icon": "icon.png",
        "overwrite_files": ["icon.png", "resources/minecraft.ttf"],
        "overwrite_folders": bundled_roots,
    }


def collect_pack_files(root: Path | None, archive_root: str, required: bool) -> list[tuple[Path, str]]:
    if root is None or not root.is_dir():
        if required:
            raise FileNotFoundError(f"Pack root not found: {root}")
        return []
    files: list[tuple[Path, str]] = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            files.append((path, f"{archive_root}/{relative}"))
    if required and not files:
        raise ValueError(f"No bundled pack files found under {root}")
    return files


def write_package(
    library: Path,
    icon: Path,
    font: Path,
    resource_packs: Path,
    behavior_packs: Path | None,
    version_header: Path,
    output: Path,
) -> None:
    if not library.is_file():
        raise FileNotFoundError(f"Library not found: {library}")
    if not icon.is_file():
        raise FileNotFoundError(f"Icon not found: {icon}")
    if not font.is_file():
        raise FileNotFoundError(f"Font not found: {font}")
    if not version_header.is_file():
        raise FileNotFoundError(f"Version header not found: {version_header}")

    resource_files = collect_pack_files(resource_packs, RESOURCE_PACK_ARCHIVE_ROOT, True)
    behavior_files = collect_pack_files(behavior_packs, BEHAVIOR_PACK_ARCHIVE_ROOT, False)
    bundled_files = resource_files + behavior_files
    bundled_roots = [RESOURCE_PACK_ARCHIVE_ROOT]
    if behavior_files:
        bundled_roots.append(BEHAVIOR_PACK_ARCHIVE_ROOT)
    manifest = build_manifest(parse_version(version_header), library.name, bundled_roots)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    expected = {"manifest.json", library.name, "icon.png", "resources/minecraft.ttf"}
    expected.update(archive_name for _, archive_name in bundled_files)

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("manifest.json", manifest_bytes)
        archive.write(library, library.name)
        archive.write(icon, "icon.png")
        archive.write(font, "resources/minecraft.ttf")
        for path, archive_name in bundled_files:
            archive.write(path, archive_name)

    with zipfile.ZipFile(output, "r") as archive:
        names = {name for name in archive.namelist() if not name.endswith("/")}
        if names != expected:
            raise RuntimeError(f"Unexpected package entries: {sorted(names)}")
        parsed = json.loads(archive.read("manifest.json"))
        if parsed != manifest:
            raise RuntimeError("Manifest verification failed")
        if archive.getinfo(library.name).file_size != library.stat().st_size:
            raise RuntimeError("Library verification failed")
        if archive.getinfo("icon.png").file_size != icon.stat().st_size:
            raise RuntimeError("Icon verification failed")
        if archive.getinfo("resources/minecraft.ttf").file_size != font.stat().st_size:
            raise RuntimeError("Font verification failed")
        for path, archive_name in bundled_files:
            if archive.getinfo(archive_name).file_size != path.stat().st_size:
                raise RuntimeError(f"Bundled pack verification failed: {archive_name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--icon", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--resource-packs", required=True, type=Path)
    parser.add_argument("--behavior-packs", type=Path)
    parser.add_argument("--version-header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        write_package(
            args.library.resolve(),
            args.icon.resolve(),
            args.font.resolve(),
            args.resource_packs.resolve(),
            args.behavior_packs.resolve() if args.behavior_packs else None,
            args.version_header.resolve(),
            args.output.resolve(),
        )
    except Exception as error:
        print(error, file=sys.stderr)
        return 1
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
