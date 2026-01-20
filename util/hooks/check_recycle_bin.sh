#!/usr/bin/env bash
set -euo pipefail

# Enforce: no direct deletions. All deletions must be moves to .recycle_bin/
# Allowed:
#  - git mv <path> .recycle_bin/<path> (shows up as R*)
#  - git rm of files already under .recycle_bin/

changed=$(git diff --cached --name-status -M)

disallowed=()
while IFS= read -r line; do
  status=$(echo "$line" | awk '{print $1}')
  case "$status" in
    D)
      path=$(echo "$line" | cut -f2)
      # Allow permanent deletion only inside recycle bin
      if [[ "$path" != .recycle_bin/* ]]; then
        disallowed+=("$path")
      fi
      ;;
    # Renames are fine; encourage rename to .recycle_bin for deletions
    R*)
      :
      ;;
    *)
      :
      ;;
  esac
done < <(printf '%s\n' "$changed")

if (( ${#disallowed[@]} > 0 )); then
  echo "ERROR: Direct deletions are not allowed." >&2
  echo "Please move files to .recycle_bin/ instead (use git mv)." >&2
  echo >&2
  printf 'Disallowed deletions (%d):\n' "${#disallowed[@]}" >&2
  for p in "${disallowed[@]}"; do
    echo "  - $p" >&2
  done
  echo >&2
  echo "Example:" >&2
  echo "  git restore --staged <file> && git mv <file> .recycle_bin/<file>" >&2
  exit 1
fi

exit 0

