#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import re
import sys
import tomllib
from pathlib import Path
from urllib import error
from urllib import request


DEFAULT_REPO = "waywallen/open-wallpaper-engine"
PLUGIN_ID = "org.waywallen.open-wallpaper-engine"
DEFAULT_ARCHES = ("x86_64", "aarch64")
PLUGIN_TEMPLATE_DIR = Path("plugins") / PLUGIN_ID
PLUGIN_TEMPLATE_NAMES = ("plugin.toml.in", "weweb-renderer.toml.in")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def read_plugin_version(manifest_path: Path) -> str:
    try:
        document = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        raise SystemExit(f"could not parse lito manifest {manifest_path}: {exc}") from exc
    version = document.get("workspace", {}).get("package", {}).get("version")
    if not isinstance(version, str) or not version:
        raise SystemExit(f"could not read workspace.package.version from {manifest_path}")
    return version


def read_toml_template(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    text = "\n".join(
        "" if re.fullmatch(r"\s*@[A-Za-z0-9_]+@\s*", line) else line
        for line in text.splitlines()
    )
    try:
        return tomllib.loads(text)
    except tomllib.TOMLDecodeError as exc:
        raise SystemExit(f"could not parse TOML template {path}: {exc}") from exc


def require_version(value, field: str, path: Path) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise SystemExit(f"{field} in {path} must be a positive integer")
    return value


def read_protocol_versions(template_paths: list[Path]) -> tuple[int, int]:
    if not template_paths:
        raise SystemExit("no plugin TOML templates configured")

    templates = [(path, read_toml_template(path)) for path in template_paths]
    plugin_template, plugin_document = templates[0]
    plugin = plugin_document.get("plugin")
    if not isinstance(plugin, dict):
        raise SystemExit(f"missing [plugin] in {plugin_template}")
    entry_version = require_version(
        plugin.get("entry_version"), "plugin.entry_version", plugin_template
    )

    spawn_versions: dict[int, list[str]] = {}
    for path, document in templates:
        renderers = document.get("renderers", {})
        if not isinstance(renderers, dict):
            raise SystemExit(f"[renderers] in {path} must be a table")
        for name, renderer in renderers.items():
            if not isinstance(renderer, dict):
                raise SystemExit(f"renderers.{name} in {path} must be a table")
            field = f"renderers.{name}.spawn_version"
            version = require_version(renderer.get("spawn_version"), field, path)
            spawn_versions.setdefault(version, []).append(f"{path}:{field}")

    if not spawn_versions:
        raise SystemExit("plugin TOML templates do not declare a renderer spawn_version")
    if len(spawn_versions) != 1:
        details = "; ".join(
            f"{version}: {', '.join(fields)}" for version, fields in sorted(spawn_versions.items())
        )
        raise SystemExit(f"renderer spawn_version values must match: {details}")

    return entry_version, next(iter(spawn_versions))


def github_request(url: str):
    req = request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "owe-update-release-manifest",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        return request.urlopen(req)
    except error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"GitHub request failed: {exc.code} {exc.reason}\n{body}") from exc
    except error.URLError as exc:
        raise SystemExit(f"GitHub request failed: {exc.reason}") from exc


def github_json(url: str):
    with github_request(url) as response:
        return json.load(response)


def fetch_release(repo: str, tag: str, latest: bool):
    releases_url = f"https://api.github.com/repos/{repo}/releases"
    if latest:
        return github_json(f"{releases_url}/latest")
    return github_json(f"{releases_url}/tags/{tag}")


def download_sha256(url: str) -> str:
    digest = hashlib.sha256()
    with github_request(url) as response:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def asset_sha256(asset: dict, download_missing_digest: bool) -> str:
    digest = asset.get("digest") or ""
    if digest.startswith("sha256:"):
        return digest.split(":", 1)[1].lower()

    name = asset.get("name", "<unknown>")
    if not download_missing_digest:
        raise SystemExit(
            f"release asset {name} has no sha256 digest in the GitHub API; "
            "rerun with --download-missing-digest to compute it from the asset"
        )

    url = asset.get("browser_download_url")
    if not url:
        raise SystemExit(f"release asset {name} has no browser_download_url")
    return download_sha256(url)


def asset_name(version: str, arch: str) -> str:
    return f"{PLUGIN_ID}-{version}-linux-{arch}.zip"


def update_manifest(
    existing: dict,
    release: dict,
    version: str,
    entry_version: int,
    spawn_version: int,
    arches: list[str],
    download_missing_digest: bool,
) -> dict:
    assets = {asset.get("name"): asset for asset in release.get("assets", [])}
    available = ", ".join(sorted(name for name in assets if name)) or "<none>"

    manifest = dict(existing)
    manifest["version"] = version
    manifest["entry_version"] = entry_version
    manifest["spawn_version"] = spawn_version

    for arch in arches:
        name = asset_name(version, arch)
        asset = assets.get(name)
        if not asset:
            raise SystemExit(f"release is missing asset {name}; available assets: {available}")

        url = asset.get("browser_download_url")
        if not url:
            raise SystemExit(f"release asset {name} has no browser_download_url")

        manifest[arch] = {
            "zip_url": url,
            "sha256": asset_sha256(asset, download_missing_digest),
        }

    return manifest


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Update update.json from GitHub release assets.")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repo, default: {DEFAULT_REPO}")
    parser.add_argument("--version", help="plugin version; defaults to workspace.package.version")
    parser.add_argument("--tag", help="release tag; defaults to v<version>")
    parser.add_argument("--latest", action="store_true", help="read the latest GitHub release instead of a tag")
    parser.add_argument("--arch", action="append", choices=DEFAULT_ARCHES, help="architecture to update; can be repeated")
    parser.add_argument("--update-json", type=Path, default=root / "update.json", help="manifest path")
    parser.add_argument(
        "--lito-manifest", type=Path, default=root / "lito.toml", help="workspace lito.toml path"
    )
    parser.add_argument(
        "--plugin-toml",
        action="append",
        type=Path,
        help=(
            "plugin TOML template containing entry/spawn versions; can be repeated; "
            "defaults to bundled templates"
        ),
    )
    parser.add_argument("--download-missing-digest", action="store_true", help="download assets to compute sha256 if API digest is missing")
    parser.add_argument("--dry-run", action="store_true", help="print the new manifest without writing update.json")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    version = args.version or read_plugin_version(args.lito_manifest)
    template_paths = args.plugin_toml or [
        repo_root() / PLUGIN_TEMPLATE_DIR / name for name in PLUGIN_TEMPLATE_NAMES
    ]
    entry_version, spawn_version = read_protocol_versions(template_paths)
    tag = args.tag or f"v{version}"

    release = fetch_release(args.repo, tag, args.latest)
    if args.latest and not args.version:
        version = str(release.get("tag_name", "")).removeprefix("v")
        if not version:
            raise SystemExit("latest release response did not include tag_name")

    if args.update_json.exists():
        existing = json.loads(args.update_json.read_text(encoding="utf-8"))
    else:
        existing = {}

    arches = args.arch or list(DEFAULT_ARCHES)
    manifest = update_manifest(
        existing,
        release,
        version,
        entry_version,
        spawn_version,
        arches,
        args.download_missing_digest,
    )
    output = json.dumps(manifest, indent=2) + "\n"

    if args.dry_run:
        sys.stdout.write(output)
    else:
        args.update_json.write_text(output, encoding="utf-8")
        print(f"updated {args.update_json} from {args.repo} {release.get('tag_name', tag)}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
