#!/usr/bin/env python3
"""Remove content from a Zensical build output that mkdocs.yml's `exclude_docs`
would have kept out of the published site.

Zensical does not support `exclude_docs` (as of 0.0.60): it builds every page
under `docs_dir` regardless of that setting. This script re-applies the same
exclusion list as a post-build step, so internal design/refactoring/RE notes
don't end up published (and search-engine-indexed) on the live docs site.
Drop this script once Zensical adds native `exclude_docs` support.

Reads the exclusion list straight out of mkdocs.yml's `exclude_docs:` block so
there is one source of truth; run after `zensical build`, before deploying.
"""

from __future__ import annotations

import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import urlparse

REPO_ROOT = Path(__file__).resolve().parent.parent
MKDOCS_YML = REPO_ROOT / "mkdocs.yml"

SITEMAP_NS = "http://www.sitemaps.org/schemas/sitemap/0.9"


def read_config_value(key: str) -> str:
    match = re.search(rf"^{key}:\s*(\S+)\s*$", MKDOCS_YML.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise SystemExit(f"mkdocs.yml has no top-level '{key}:' key")
    return match.group(1)


def read_exclude_docs() -> list[str]:
    # A plain text scan, not a YAML parse: mkdocs.yml uses a
    # `!!python/object/apply:...` tag elsewhere that only PyYAML's unsafe
    # loader understands, and all we need here is this one literal block.
    text = MKDOCS_YML.read_text(encoding="utf-8")
    match = re.search(r"^exclude_docs:\s*\|\s*\n((?:[ \t]+\S.*\n?)*)", text, re.MULTILINE)
    if not match:
        raise SystemExit("mkdocs.yml has no 'exclude_docs:' block to mirror")
    return [line.strip() for line in match.group(1).splitlines() if line.strip()]


def output_relpath_for(entry: str) -> str:
    """Map an exclude_docs entry to its built, directory-URL output path."""
    if entry.endswith("/"):
        return entry
    if entry.endswith(".md"):
        return entry[: -len(".md")] + "/"
    raise SystemExit(f"exclude_docs entry '{entry}' is neither a dir/ nor a .md file — teach this script about it")


def prune_site_dir(site_dir: Path, relpaths: list[str]) -> list[str]:
    removed = []
    for relpath in relpaths:
        target = site_dir / relpath
        if target.is_dir():
            shutil.rmtree(target)
            removed.append(relpath)
        else:
            print(f"note: '{relpath}' not present in build output (already excluded?) — skipping", file=sys.stderr)
    return removed


def prune_sitemap(site_dir: Path, site_url: str, relpaths: list[str]) -> int:
    sitemap_path = site_dir / "sitemap.xml"
    if not sitemap_path.is_file():
        return 0

    site_path_prefix = urlparse(site_url).path  # e.g. "/aqualink-automate/"
    ET.register_namespace("", SITEMAP_NS)
    tree = ET.parse(sitemap_path)
    root = tree.getroot()

    kept, dropped = [], 0
    for url_el in list(root):
        loc_el = url_el.find(f"{{{SITEMAP_NS}}}loc")
        page_path = urlparse(loc_el.text).path
        if page_path.startswith(site_path_prefix):
            page_path = page_path[len(site_path_prefix) :]
        if any(page_path.startswith(relpath) for relpath in relpaths):
            root.remove(url_el)
            dropped += 1

    if dropped:
        tree.write(sitemap_path, encoding="utf-8", xml_declaration=True)
    return dropped


def main() -> None:
    site_dir = REPO_ROOT / read_config_value("site_dir")
    site_url = read_config_value("site_url")
    if not site_dir.is_dir():
        raise SystemExit(f"site_dir '{site_dir}' does not exist — run the build first")

    relpaths = [output_relpath_for(entry) for entry in read_exclude_docs()]
    removed_dirs = prune_site_dir(site_dir, relpaths)
    dropped_urls = prune_sitemap(site_dir, site_url, relpaths)

    print(f"Pruned {len(removed_dirs)}/{len(relpaths)} excluded output dirs, {dropped_urls} sitemap entries.")


if __name__ == "__main__":
    main()
