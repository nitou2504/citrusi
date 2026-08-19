#!/usr/bin/env python3
"""Install the selected Citrusi brand assets.

The Citrus Slice concept is the current production draft. The other concepts
under docs/branding/citrusi/ remain available for comparison.

Run from the repository root:
    python3 tools/brand/gen-brand.py
"""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
BRAND = ROOT / "docs" / "branding" / "citrusi"
ICON = BRAND / "01-slice-icon.png"
BANNER = BRAND / "01-slice-banner.png"

TARGETS = {
    ICON: (ROOT / "icon.png", ROOT / "ctr-template" / "icon.png"),
    BANNER: (ROOT / "cia" / "banner.png", ROOT / "ctr-template" / "banner.png"),
}


def main():
    for source, targets in TARGETS.items():
        if not source.is_file():
            raise SystemExit(f"missing brand source: {source}")
        for target in targets:
            shutil.copyfile(source, target)
            print(f"brand asset copied: {target.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
