#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <input.step> [work_dir]" >&2
  exit 2
fi

INPUT_STEP="$1"
WORK_DIR="${2:-/tmp/stp_face_isolation}"
PROBE_BIN="$WORK_DIR/step_import_probe"
SANITIZED_STEP="$WORK_DIR/sanitized.step"
LOG_FILE="$WORK_DIR/isolation.log"

mkdir -p "$WORK_DIR"
: > "$LOG_FILE"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
MODEL2TILE_BIN="$ROOT_DIR/model2tile"

# Replace a contiguous face-ID range with a known-small valid face payload.
sanitize_range() {
  local lo="$1"
  local hi="$2"

  perl -e '
    use strict;
    use warnings;

    my ($in, $out, $lo, $hi) = @ARGV;
    open my $IN, q{<}, $in or die "open in failed: $!";
    open my $OUT, q{>}, $out or die "open out failed: $!";

    my $skip = 0;
    my $sq = chr(39);
    my $stub = "COMPLEX_TRIANGULATED_FACE(${sq}${sq},#14338,3,((-0.829360365867615,0.0109848361462355,0.558606028556824)),\$,(1,2,3),(),((1)));";

    while (my $line = <$IN>) {
      if (!$skip && $line =~ /^#(\d+)=COMPLEX_TRIANGULATED_FACE\(/) {
        my $id = $1;
        if ($id >= $lo && $id <= $hi) {
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
  ' "$INPUT_STEP" "$SANITIZED_STEP" "$lo" "$hi"
}

probe() {
  local step_path="$1"
  set +e
  # macOS-friendly timeout wrapper using perl alarm.
  perl -e 'alarm shift; exec @ARGV' 120 \
    "$MODEL2TILE_BIN" -v -o "$WORK_DIR/out" "$step_path" \
    >"$WORK_DIR/probe.out" 2>"$WORK_DIR/probe.err"
  local ec=$?
  set -e
  return "$ec"
}

# Candidate face IDs come from TESSELLATED_SOLID references.
FACE_IDS=()
while IFS= read -r id; do
  FACE_IDS+=("$id")
done < <(
  grep -oE 'TESSELLATED_SOLID\([^)]*\(#([0-9]+)\)' "$INPUT_STEP" \
    | sed -E 's/.*\(#([0-9]+)\).*/\1/'
)

if [[ ${#FACE_IDS[@]} -eq 0 ]]; then
  echo "No tessellated solid face references found." >&2
  exit 1
fi

# sort + unique
SORTED_IDS=()
while IFS= read -r id; do
  SORTED_IDS+=("$id")
done < <(printf '%s\n' "${FACE_IDS[@]}" | sort -n | uniq)
FACE_IDS=("${SORTED_IDS[@]}")

last_index=$((${#FACE_IDS[@]} - 1))
echo "candidate_faces=${#FACE_IDS[@]} first=${FACE_IDS[0]} last=${FACE_IDS[$last_index]}" | tee -a "$LOG_FILE"

if [[ ! -x "$MODEL2TILE_BIN" ]]; then
  echo "Missing executable: $MODEL2TILE_BIN" >&2
  exit 1
fi

# Validate baseline reproduces segfault in probe.
if probe "$INPUT_STEP"; then
  echo "baseline_status=ok(no-crash)" | tee -a "$LOG_FILE"
  echo "Original did not segfault under timeout window; cannot isolate by crash bisect." | tee -a "$LOG_FILE"
  exit 0
else
  BASE_EC=$?
  echo "baseline_probe_exit=$BASE_EC" | tee -a "$LOG_FILE"
  if [[ "$BASE_EC" -ne 139 ]]; then
    echo "Baseline is not segfault (likely timeout/non-crash); cannot isolate exact crashing face reliably." | tee -a "$LOG_FILE"
    exit 1
  fi
fi

# Binary isolate by replacing ranges and checking if crash disappears.
lo=0
hi=$((${#FACE_IDS[@]} - 1))

while [[ $lo -lt $hi ]]; do
  mid=$(((lo + hi) / 2))

  face_lo="${FACE_IDS[$lo]}"
  face_mid="${FACE_IDS[$mid]}"

  sanitize_range "$face_lo" "$face_mid"

  if probe "$SANITIZED_STEP"; then
    echo "range [$face_lo,$face_mid] contains crash face" | tee -a "$LOG_FILE"
    hi="$mid"
  else
    ec=$?
    if [[ "$ec" -eq 139 ]]; then
      echo "range [$face_lo,$face_mid] clean; crash in upper half" | tee -a "$LOG_FILE"
      lo=$((mid + 1))
    else
      echo "range [$face_lo,$face_mid] inconclusive (exit=$ec)" | tee -a "$LOG_FILE"
      exit 1
    fi
  fi

done

bad_face="${FACE_IDS[$lo]}"
echo "isolated_face_id=$bad_face" | tee -a "$LOG_FILE"

# Emit a patched file with only that face stubbed.
sanitize_range "$bad_face" "$bad_face"
FIXED_STEP="$WORK_DIR/fixed_face_${bad_face}.step"
cp "$SANITIZED_STEP" "$FIXED_STEP"

echo "fixed_file=$FIXED_STEP" | tee -a "$LOG_FILE"

if probe "$FIXED_STEP"; then
  echo "fixed_probe=success" | tee -a "$LOG_FILE"
else
  ec=$?
  echo "fixed_probe=still_fails exit=$ec" | tee -a "$LOG_FILE"
fi

echo "Done. See $LOG_FILE"
