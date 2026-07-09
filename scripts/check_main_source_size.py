from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
LIMIT = 100 * 1024


def main() -> int:
    files = sorted(MAIN.rglob("*.c")) + sorted(MAIN.rglob("*.h"))
    total = sum(path.stat().st_size for path in files)
    print(f"main source files: {len(files)}")
    print(f"main source bytes: {total}")
    print(f"target bytes:      {LIMIT}")
    print(f"over target:       {max(0, total - LIMIT)}")
    for path in files:
        print(f"{path.relative_to(ROOT)} {path.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

