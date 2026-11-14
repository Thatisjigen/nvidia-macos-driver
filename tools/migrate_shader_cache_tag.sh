#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 kevineduardo
# migrate_shader_cache_tag.sh [--dry-run] [--store DIR] <old tag> <new tag>
# migrate_shader_cache_tag.sh --relink --store DIR
#
# Keeps the shader caches across a change of the cache's build tag when the translator itself did not change (the
# plugin that takes the tag from the translator's content instead of the whole signed file, plugin/nvmtl_translator_tag.h).
# Without it every process sweeps its cache on its first start with the new plugin, and WindowServer waits on hundreds
# of translations at the next boot (two minutes of black screen on an RTX 5060 Ti).
#
# Every cache entry carries the tag in its NAME only (<digest>.<kind>.<tag><modes>.c3.spv, idx2-<lib>.<tag><modes>.plist;
# the contents are the SPIR-V and a footer, or the index). For each entry of <old tag> in every nvmtl/spvcache and
# nvmtl/aircache folder on this Mac, this makes a hard link under the name with <new tag>: no copy, no space, and the old
# name stays for processes still running the old plugin. Tags are "sha256-<64 hex>"; tools/translator_tag.c prints both
# for a translator. Run as root (the caches belong to several users), BEFORE installing the new plugin.
#
# --store DIR also keeps a hard link of every migrated entry in DIR with a manifest. A process that still runs the old
# plugin may sweep the new names as "another build's" when it first uses its cache; --relink puts back whatever is
# missing. Run it once more just before the restart.
set -uo pipefail
DRY=; STORE=; RELINK=
# where caches live: the shared folder, daemons' and users' cache folders, app containers. NVMTL_CACHE_ROOTS overrides (tests).
ROOTS=${NVMTL_CACHE_ROOTS:-/Library/GPUBundles/nvmtl /private/var/db /private/var/folders /private/var/root /Users}
while [ $# -gt 0 ]; do case $1 in
  --dry-run) DRY=1; shift;; --store) STORE=$2; shift 2;; --relink) RELINK=1; shift;; *) break;; esac; done
if [ -n "$RELINK" ]; then
  [ -n "$STORE" ] && [ -f "$STORE/manifest.tsv" ] || { echo "usage: $0 --relink --store DIR (with a manifest.tsv)" >&2; exit 2; }
  put=0; ok=0; lost=0
  while IFS=$'\t' read -r n dest; do
    [ -n "$n" ] || continue
    if [ -e "$dest" ]; then ok=$((ok+1)); continue; fi
    if [ -d "$(dirname "$dest")" ] && ln "$STORE/$n" "$dest" 2>/dev/null; then put=$((put+1)); else lost=$((lost+1)); fi
  done < "$STORE/manifest.tsv"
  echo "relink: $ok still there, $put put back, $lost could not be put back"
  exit 0
fi
OLD=${1:-}; NEW=${2:-}
[[ "$OLD" =~ ^sha256-[0-9a-f]{64}$ && "$NEW" =~ ^sha256-[0-9a-f]{64}$ ]] || { sed -n '4,5p' "$0" | sed 's/^# //' >&2; exit 2; }
[ "$OLD" != "$NEW" ] || { echo "the tags are the same: nothing to do"; exit 0; }
[ -z "$STORE" ] || [ -n "$DRY" ] || { mkdir -p "$STORE" && : > "$STORE/manifest.tsv"; }
dirs=0; linked=0; had=0; failed=0; n=0
while IFS= read -r -d '' d; do
  dirs=$((dirs+1))
  while IFS= read -r -d '' f; do
    base=${f##*/}; dest=${f%/*}/${base//$OLD/$NEW}
    if [ -e "$dest" ]; then had=$((had+1))
    elif [ -n "$DRY" ]; then linked=$((linked+1)); continue
    elif ln "$f" "$dest" 2>/dev/null; then linked=$((linked+1))
    else failed=$((failed+1)); continue; fi
    if [ -n "$STORE" ] && [ -z "$DRY" ]; then n=$((n+1)); ln "$dest" "$STORE/$n" 2>/dev/null && printf '%s\t%s\n' "$n" "$dest" >> "$STORE/manifest.tsv"; fi
  done < <(find "$d" -maxdepth 1 -type f -name "*$OLD*" -print0 2>/dev/null)
done < <(find $ROOTS -xdev \( -name spvcache -o -name aircache \) -type d -path '*/nvmtl/*' -print0 2>/dev/null)
echo "${DRY:+dry run: }$dirs cache folders: $linked entries ${DRY:+would be }linked to the new tag, $had already there, $failed failed${STORE:+; store $STORE}"
[ "$failed" = 0 ]
