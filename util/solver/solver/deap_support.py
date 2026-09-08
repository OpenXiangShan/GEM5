from __future__ import annotations

from pathlib import Path
import sys


def import_deap():
    try:
        from deap import base, creator, tools  # type: ignore
        return base, creator, tools
    except ImportError:
        repo_root = Path(__file__).resolve().parents[3]
        site_packages = next(
            (repo_root / ".venv-solver" / "lib").glob("python*/site-packages"),
            None,
        )
        if site_packages is not None and str(site_packages) not in sys.path:
            sys.path.insert(0, str(site_packages))
            from deap import base, creator, tools  # type: ignore
            return base, creator, tools
        raise
