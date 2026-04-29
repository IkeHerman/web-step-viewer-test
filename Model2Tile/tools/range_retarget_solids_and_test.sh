#!/usr/bin/env bash
set -euo pipefail

TIMEOUT_SECS="${TIMEOUT_SECS:-700}"
GOOD_FACE_ID="${GOOD_FACE_ID:-14086}"

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

  my ($in, $out, $lo, $hi, $good) = @ARGV;
  open my $IN, q{<}, $in or die "open in failed: $!";
  open my $OUT, q{>}, $out or die "open out failed: $!";

  my $in_tess_solid = 0;

  while (my $line = <$IN>) {
    if ($line =~ /^#\d+=TESSELLATED_SOLID\(/) {
      $in_tess_solid = 1;
    }

    if ($in_tess_solid) {
      $line =~ s/\(#(\d+)\),\$\);/
        my $id = $1;
        if ($id >= $lo && $id <= $hi) {
          "(#" . $good . "),\$);";
        } else {
          "(#" . $id . "),\$);";
        }
      /eg;

      if ($line =~ /;\s*$/) {
        $in_tess_solid = 0;
      }
    }

    print {$OUT} $line;
  }

  close $IN;
  close $OUT;
' "$INPUT_STEP" "$OUT_STEP" "$ID_LO" "$ID_HI" "$GOOD_FACE_ID"

set +e
perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_SECS" \
  ./model2tile -v -o "$OUT_DIR" "$OUT_STEP" >"$OUT_DIR/run.out" 2>"$OUT_DIR/run.err"
EC=$?
set -e

echo "$EC"
