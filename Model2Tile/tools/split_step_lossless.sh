#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 <input.step> <out_dir> [num_shards]" >&2
  echo "Example: $0 model.step /tmp/model_shards 32" >&2
  exit 2
fi

INPUT_STEP="$1"
OUT_DIR="$2"
NUM_SHARDS="${3:-32}"

if [[ ! -f "$INPUT_STEP" ]]; then
  echo "Input file not found: $INPUT_STEP" >&2
  exit 1
fi

if ! [[ "$NUM_SHARDS" =~ ^[0-9]+$ ]] || [[ "$NUM_SHARDS" -le 0 ]]; then
  echo "num_shards must be a positive integer" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

TMP_DIR="$(mktemp -d /tmp/step_lossless_split.XXXXXX)"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

HEADER_FILE="$TMP_DIR/header.part21"
DATA_FILE="$TMP_DIR/data.section"
FOOTER_FILE="$TMP_DIR/footer.part21"
ENTITIES_FILE="$TMP_DIR/entities.txt"
MANIFEST_FILE="$OUT_DIR/manifest.txt"

# Split into HEADER / DATA body / FOOTER while preserving exact text.
awk -v h="$HEADER_FILE" -v d="$DATA_FILE" -v f="$FOOTER_FILE" '
  BEGIN { in_data = 0; after_data = 0; }
  {
    if (!in_data && !after_data) {
      print > h;
      if ($0 ~ /^DATA;[[:space:]]*$/) {
        in_data = 1;
      }
      next;
    }

    if (in_data) {
      if ($0 ~ /^ENDSEC;[[:space:]]*$/) {
        print > f;
        in_data = 0;
        after_data = 1;
      } else {
        print > d;
      }
      next;
    }

    print > f;
  }
' "$INPUT_STEP"

if [[ ! -s "$HEADER_FILE" || ! -s "$FOOTER_FILE" ]]; then
  echo "Failed to parse STEP structure (missing header/footer)" >&2
  exit 1
fi

# Group multi-line DATA entities by semicolon terminator.
awk '
  BEGIN { rec = ""; }
  {
    rec = rec $0 "\n";
    if ($0 ~ /;[[:space:]]*$/) {
      printf "%s\f", rec;
      rec = "";
    }
  }
  END {
    if (length(rec) > 0) {
      printf "%s\f", rec;
    }
  }
' "$DATA_FILE" > "$ENTITIES_FILE"

TOTAL_ENTITIES=$(awk -v RS='\f' 'NF { c++ } END { print c + 0 }' "$ENTITIES_FILE")
if [[ "$TOTAL_ENTITIES" -eq 0 ]]; then
  echo "No DATA entities found to split" >&2
  exit 1
fi

# Keep shard count <= entity count.
if [[ "$NUM_SHARDS" -gt "$TOTAL_ENTITIES" ]]; then
  NUM_SHARDS="$TOTAL_ENTITIES"
fi

BASE=$((TOTAL_ENTITIES / NUM_SHARDS))
REM=$((TOTAL_ENTITIES % NUM_SHARDS))

{
  echo "input=$INPUT_STEP"
  echo "total_entities=$TOTAL_ENTITIES"
  echo "num_shards=$NUM_SHARDS"
  echo "distribution_base=$BASE"
  echo "distribution_remainder=$REM"
} > "$MANIFEST_FILE"

entity_index=1
for ((i = 1; i <= NUM_SHARDS; i++)); do
  count="$BASE"
  if [[ "$i" -le "$REM" ]]; then
    count=$((count + 1))
  fi

  out_file="$OUT_DIR/shard_$(printf '%02d' "$i").step"
  cp "$HEADER_FILE" "$out_file"

  if [[ "$count" -gt 0 ]]; then
    start="$entity_index"
    end=$((entity_index + count - 1))
    awk -v RS='\f' -v ORS='' -v s="$start" -v e="$end" '
      NR >= s && NR <= e { print $0 }
    ' "$ENTITIES_FILE" >> "$out_file"
    entity_index=$((end + 1))
  fi

  cat "$FOOTER_FILE" >> "$out_file"

  {
    printf "shard_%02d_entities=%d\n" "$i" "$count"
    printf "shard_%02d_file=%s\n" "$i" "$out_file"
  } >> "$MANIFEST_FILE"
done

# Integrity check: sum shard entities must equal original entity count.
SUM_SHARD_ENTITIES=$(awk -F= '/^shard_[0-9][0-9]_entities=/{sum += $2} END { print sum + 0 }' "$MANIFEST_FILE")
if [[ "$SUM_SHARD_ENTITIES" -ne "$TOTAL_ENTITIES" ]]; then
  echo "Integrity check failed: shard entity sum ($SUM_SHARD_ENTITIES) != total entities ($TOTAL_ENTITIES)" >&2
  exit 1
fi

# Stronger check: compare sorted list of entity IDs across original vs all shards.
ORIG_IDS="$TMP_DIR/orig_ids.txt"
SHARD_IDS="$TMP_DIR/shard_ids.txt"

grep -oE '^#[0-9]+' "$DATA_FILE" | sort -u > "$ORIG_IDS"
grep -h -oE '^#[0-9]+' "$OUT_DIR"/shard_*.step | sort -u > "$SHARD_IDS"

if ! cmp -s "$ORIG_IDS" "$SHARD_IDS"; then
  echo "Integrity check failed: entity ID sets differ between original and shards" >&2
  exit 1
fi

{
  echo "integrity_entity_sum_ok=1"
  echo "integrity_entity_id_set_ok=1"
} >> "$MANIFEST_FILE"

echo "Created $NUM_SHARDS lossless shard files in: $OUT_DIR"
echo "Manifest: $MANIFEST_FILE"
ls -lh "$OUT_DIR"/shard_*.step | sed -n '1,200p'
