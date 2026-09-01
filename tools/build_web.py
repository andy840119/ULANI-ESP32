#!/usr/bin/env python3
"""Build the web UI and stage it for the firmware.

Runs `npm install` (first time only) and `npm run build` in web/, then gzips
web/dist into components/web_server/www, where CMake embeds it.

Usage:
    python tools/build_web.py [--skip-npm]
"""

import argparse
import gzip
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
DIST = WEB / "dist"
WWW = ROOT / "components" / "web_server" / "www"

# Must match the extern symbols in components/web_server/src/web_server.c.
ASSETS = ["index.html", "app.js", "app.css"]


def run(cmd: list[str], cwd: Path) -> None:
    print(f"$ {' '.join(cmd)}", flush=True)
    # npm is a .cmd shim on Windows, so go through the shell there.
    subprocess.run(cmd, cwd=cwd, check=True, shell=(sys.platform == "win32"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-npm", action="store_true",
                        help="reuse web/dist as-is instead of rebuilding")
    args = parser.parse_args()

    if not args.skip_npm:
        if not (WEB / "node_modules").exists():
            run(["npm", "install"], WEB)
        run(["npm", "run", "build"], WEB)

    if not DIST.exists():
        print(f"error: {DIST} does not exist", file=sys.stderr)
        return 1

    WWW.mkdir(parents=True, exist_ok=True)
    for stale in WWW.glob("*.gz"):
        stale.unlink()

    total = 0
    for name in ASSETS:
        src = DIST / name
        if not src.exists():
            print(f"error: {src} missing -- check vite.config.ts output names",
                  file=sys.stderr)
            return 1
        dst = WWW / f"{name}.gz"
        with src.open("rb") as fin, gzip.open(dst, "wb", compresslevel=9) as fout:
            shutil.copyfileobj(fin, fout)
        size = dst.stat().st_size
        total += size
        print(f"  {name}: {src.stat().st_size} -> {size} bytes")

    print(f"staged {len(ASSETS)} files ({total} bytes) into {WWW}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
