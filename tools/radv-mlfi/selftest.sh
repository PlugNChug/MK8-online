#!/bin/sh
# Self-test for tools/radv-mlfi.  Needs vulkan-headers, a C compiler and spirv-tools.
#
#   1. builds `repro`;
#   2. assembles the minimal cases and checks the two controls compile;
#   3. runs the fixer on the two invalid 16-bit cases and on the 64-bit case and checks the
#      rewritten modules are valid SPIR-V and compile;
#   4. runs the unmodified crash cases: the expected result is either a segfault (driver bug
#      present) or a clean VK_ERROR_UNKNOWN (driver fixed).  Anything else is a failure.
#
# The script never needs AMD's DLL.  Exit status 0 means "the tools and the driver behave as
# expected for this RADV", not "the driver is fixed".
set -u
cd "$(dirname "$0")"

say() { printf '%s\n' "$*"; }
fail=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/radv-mlfi-selftest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

say "== build"
./build.sh >/dev/null || exit 1
say "ok"

say "== assemble cases"
for c in storage16_unknown storage16_rg16f sampled16_unknown sampled64_unknown storage64_r64ui; do
   spirv-as --target-env vulkan1.2 "cases/$c.spvasm" -o "$tmp/$c.spv" || exit 1
done
say "ok"

say "== controls (must compile)"
for c in storage16_rg16f storage64_r64ui; do
   if ./repro --module "$tmp/$c.spv" 2>/dev/null | grep -q '^pipeline: OK'; then
      say "PASS $c"
   else
      say "FAIL $c (expected pipeline: OK)"; fail=1
   fi
done

say "== fixer on the invalid modules (must become valid and compile)"
# The fixer rewrites 16-bit image sampled types; the 64-bit case needs the storage form.
for c in storage16_unknown sampled16_unknown; do
   python3 fix_image_types.py --in "$tmp/$c.spv" --out "$tmp/$c.fixed.spv" >/dev/null || { fail=1; continue; }
   spirv-val --target-env vulkan1.2 "$tmp/$c.fixed.spv" >/dev/null 2>&1 || { say "FAIL $c: fixed module invalid"; fail=1; continue; }
   if ./repro --module "$tmp/$c.fixed.spv" 2>/dev/null | grep -q '^pipeline: OK'; then
      say "PASS $c -> fixed module compiles"
   else
      say "FAIL $c: fixed module does not compile"; fail=1
   fi
done
python3 fix_image_types.py --in "$tmp/sampled64_unknown.spv" --out "$tmp/sampled64_unknown.fixed.spv" \
   --i64-sampled-to-storage >/dev/null || fail=1
if spirv-val --target-env vulkan1.2 "$tmp/sampled64_unknown.fixed.spv" >/dev/null 2>&1 &&
   ./repro --module "$tmp/sampled64_unknown.fixed.spv" 2>/dev/null | grep -q '^pipeline: OK'; then
   say "PASS sampled64_unknown -> fixed module compiles"
else
   say "FAIL sampled64_unknown: fixed module does not compile"; fail=1
fi

say "== unmodified crash cases (segfault means the RADV NULL-deref bug is present)"
for c in storage16_unknown sampled16_unknown sampled64_unknown; do
   timeout 60 ./repro --module "$tmp/$c.spv" >/dev/null 2>&1
   rc=$?
   case $rc in
   139|134) say "KNOWN-BUG $c: driver crashed (exit $rc)" ;;
   3)       say "FIXED $c: driver rejected the module cleanly (exit 3)" ;;
   *)       say "FAIL $c: unexpected exit $rc"; fail=1 ;;
   esac
done

if [ "$fail" -eq 0 ]; then say "== self-test passed"; else say "== self-test FAILED"; fi
exit "$fail"
