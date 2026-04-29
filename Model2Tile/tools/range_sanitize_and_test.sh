#!/usr/bin/env bash
set -euo pipefail

# Max importer runtime for classification. On timeout, perl alarm exits non-zero
# (typically 142 on macOS), which we treat as non-segfault for triage purposes.
TIMEOUT_SECS="${TIMEOUT_SECS:-180}"

if [[ $# -ne 5 ]]; then
  echo "Usage: $0 <input.step> <out.step> <id_lo> <id_hi> <out_dir>" >&2
  exit 2
fi

INPUT_STEP="$1"
OUT_STEP="$2"
ID_LO="$3"
ID_HI="$4"
OUT_DIR="$5"

mkdir -p "$(dirname "$OUT_STEP")"
mkdir -p "$OUT_DIR"

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
' "$INPUT_STEP" "$OUT_STEP" "$ID_LO" "$ID_HI"

set +e
perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_SECS" \
  ./model2tile -v -o "$OUT_DIR" "$OUT_STEP" >"$OUT_DIR/run.out" 2>"$OUT_DIR/run.err"
EC=$?
set -e

echo "$EC"
