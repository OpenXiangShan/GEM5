from __future__ import annotations

import re


_SEGMENT_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(\[[0-9]+\])*$")
_INDEX_RE = re.compile(r"\[([0-9]+)\]")


class PathResolutionError(RuntimeError):
    pass


def _parse_segment(segment: str) -> tuple[str, list[int]]:
    match = _SEGMENT_RE.match(segment)
    if not match:
        raise PathResolutionError(f"unsupported path segment {segment!r}")
    attr = match.group(1)
    indices = [int(item) for item in _INDEX_RE.findall(segment)]
    return attr, indices


def resolve_object(root, path: str):
    if not path:
        return root
    current = root
    segments = [segment for segment in path.split(".") if segment]
    if segments and segments[0] == "root":
        segments = segments[1:]
    for segment in segments:
        attr, indices = _parse_segment(segment)
        try:
            current = getattr(current, attr)
        except AttributeError as exc:
            raise PathResolutionError(
                f"failed to resolve attribute {attr!r} in {path!r}"
            ) from exc
        for index in indices:
            try:
                current = current[index]
            except Exception as exc:  # pragma: no cover - gem5 vectors are not plain lists
                raise PathResolutionError(
                    f"failed to index {attr!r}[{index}] while resolving {path!r}"
                ) from exc
    return current


def split_target(target: str) -> tuple[str, str]:
    parts = target.rsplit(".", 1)
    if len(parts) != 2 or not parts[0] or not parts[1]:
        raise PathResolutionError(f"target must look like object.param, got {target!r}")
    return parts[0], parts[1]


def resolve_target(root, target: str):
    owner_path, param_name = split_target(target)
    owner = resolve_object(root, owner_path)
    return owner, owner_path, param_name
