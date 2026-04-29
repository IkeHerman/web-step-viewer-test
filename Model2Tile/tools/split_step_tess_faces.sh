#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "Usage: $0 <input.step> <out_dir> [faces_per_shard] [stub_face_id]" >&2
  echo "Example: $0 model.step /tmp/step_shards 32 14338" >&2
  exit 2
fi

INPUT_STEP="$1"
OUT_DIR="$2"
FACES_PER_SHARD="${3:-32}"
STUB_FACE_ID="${4:-14338}"

if [[ ! -f "$INPUT_STEP" ]]; then
  echo "Input file not found: $INPUT_STEP" >&2
  exit 1
fi

if ! [[ "$FACES_PER_SHARD" =~ ^[0-9]+$ ]] || [[ "$FACES_PER_SHARD" -le 0 ]]; then
  echo "faces_per_shard must be a positive integer" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

# Discover all COMPLEX_TRIANGULATED_FACE ids without SIGPIPE-prone head/tail pipes.
read -r FIRST_ID LAST_ID COUNT < <(
  awk '
    BEGIN { first = 0; last = 0; count = 0; }
    /^#[0-9]+=COMPLEX_TRIANGULATED_FACE\(/ {
      id = $0;
      sub(/^#/, "", id);
      sub(/=.*/, "", id);
      if (first == 0) first = id;
      last = id;
      count++;
    }
    END { printf "%s %s %s\n", first, last, count; }
  ' "$INPUT_STEP"
)

if [[ -z "$FIRST_ID" || -z "$LAST_ID" || "$COUNT" -eq 0 ]]; then
  echo "No COMPLEX_TRIANGULATED_FACE entries found in: $INPUT_STEP" >&2
  exit 1
fi

# Stub keeps entity id stable but replaces payload with minimal valid shape.
# This is much less destructive than removing entities and keeps references valid.
make_shard() {
  local keep_lo="$1"
  local keep_hi="$2"
  local out_step="$3"

  perl -e '
    use strict;
    use warnings;

    my ($in, $out, $keep_lo, $keep_hi, $stub_ref) = @ARGV;
    open my $IN, q{<}, $in or die "open in failed: $!";
    open my $OUT, q{>}, $out or die "open out failed: $!";

    my $skip = 0;
    my $sq = chr(39);
    my $stub = "COMPLEX_TRIANGULATED_FACE(${sq}${sq},#${stub_ref},3,((-0.829360365867615,0.0109848361462355,0.558606028556824)),\$,(1,2,3),(),((1)));";

    while (my $line = <$IN>) {
      if (!$skip && $line =~ /^#(\d+)=COMPLEX_TRIANGULATED_FACE\(/) {
        my $id = $1;

        if ($id < $keep_lo || $id > $keep_hi) {
          print {$OUT} "#$id=$stub\n";
          $skip = 1;
          next;
        }
      }

      if ($skip) {
        if ($line =~ /;\s*$/) {
          $skip = 0;
        }
        next;
      }

      print {$OUT} $line;
    }

    close $IN;
    close $OUT;
  ' "$INPUT_STEP" "$out_step" "$keep_lo" "$keep_hi" "$STUB_FACE_ID"
}

echo "Input: $INPUT_STEP"
echo "Complex faces: count=$COUNT first=$FIRST_ID last=$LAST_ID"
echo "Shard size: $FACES_PER_SHARD"
echo "Output dir: $OUT_DIR"

index=1
current_lo="$FIRST_ID"

while [[ "$current_lo" -le "$LAST_ID" ]]; do
  current_hi=$((current_lo + FACES_PER_SHARD - 1))
  if [[ "$current_hi" -gt "$LAST_ID" ]]; then
    current_hi="$LAST_ID"
  fi

  out_file="$OUT_DIR/shard_${index}_faces_${current_lo}_${current_hi}.step"
  echo "Writing shard $index: keep [$current_lo,$current_hi] -> $out_file"
  make_shard "$current_lo" "$current_hi" "$out_file"

  current_lo=$((current_hi + 1))
  index=$((index + 1))
done

echo "Done. Generated $((index - 1)) shard files."
ls -lh "$OUT_DIR"/*.step | sed -n '1,200p'
