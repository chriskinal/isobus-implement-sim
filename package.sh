#!/usr/bin/env bash
# Packages a build into a release tarball (docs/PLAN.md phase 8): the binaries, the profiles,
# the pools, the scenario harness and the documents, so a profile runs from the tarball alone.
#
#     package.sh <build directory> <version>      e.g. package.sh build v0.1.0
set -euo pipefail
build=${1:?build directory}
version=${2:?version}
arch=$(uname -m)
name="isobus-implement-sim-${version}-linux-${arch}"
out="dist/${name}"
rm -rf "$out" && mkdir -p "$out/bin" "$out/tests"
for binary in implement vt_stub tc_stub aux_input ddop_check; do
  install -m 0755 "$build/$binary" "$out/bin/$binary"
done
cp -r profiles pools docs "$out/"
cp README.md LICENSE "$out/"
cp tests/harness.py tests/scenario_*.py tests/phase0_vcan.sh "$out/tests/"
cat > "$out/RELEASE.md" <<NOTE
# isobus-implement-sim ${version}, Linux ${arch}

Built from $(git rev-parse --short HEAD 2>/dev/null || echo "unknown") on $(date -u +%Y-%m-%d).

    sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
    bin/implement --profile profiles/sprayer-24.json --interface vcan0 --port 0

Then connect your task controller or terminal to the same interface, or a real adapter's
(\`--interface can0\`). Commands are lines on stdin or the TCP port: \`help\` lists them;
\`status\` shows the machine. See README.md and docs/PLAN.md.

The scenarios in tests/ run against the bundled stubs:

    python3 tests/scenario_vt.py bin/implement bin/vt_stub vcan0
    python3 tests/scenario_rates.py bin/implement bin/tc_stub vcan0
NOTE
mkdir -p dist
tar -C dist -czf "dist/${name}.tar.gz" "$name"
echo "dist/${name}.tar.gz"
du -h "dist/${name}.tar.gz" | cut -f1
