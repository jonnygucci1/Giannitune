#!/bin/bash
# Giannitune macOS Installer — pkgbuild script
#
# Builds a .pkg installer that places:
#   - VST3  → /Library/Audio/Plug-Ins/VST3/Giannitune.vst3
#   - AU    → /Library/Audio/Plug-Ins/Components/Giannitune.component
#
# UNSIGNED. Users must right-click → Open on first run to bypass
# Gatekeeper. Documented in INSTALL.md.
#
# Prerequisites:
#   1. Plugin built: cmake --build build --config Release
#   2. macOS with pkgbuild + productbuild (included in Xcode Command
#      Line Tools — run `xcode-select --install` if missing)
#
# Usage (run from repo root):
#   ./installer/macos/build_pkg.sh
#
# Output:
#   installer/macos/Output/Giannitune-v1.2.1-macOS.pkg

set -e

VERSION="1.2.1"
IDENTIFIER="com.gianni.giannitune"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/Giannitune_artefacts/Release"
OUT_DIR="${REPO_ROOT}/installer/macos/Output"
STAGING="${REPO_ROOT}/installer/macos/_staging"

# Pre-flight
if [ ! -d "${BUILD_DIR}/VST3/Giannitune.vst3" ]; then
    echo "ERROR: VST3 not found at ${BUILD_DIR}/VST3/Giannitune.vst3"
    echo "Build it first: cmake --build build --config Release --target Giannitune_VST3"
    exit 1
fi
if [ ! -d "${BUILD_DIR}/AU/Giannitune.component" ]; then
    echo "ERROR: AU not found at ${BUILD_DIR}/AU/Giannitune.component"
    echo "Build it first: cmake --build build --config Release --target Giannitune_AU"
    exit 1
fi

echo "=== Building Giannitune v${VERSION} macOS installer ==="

# Clean + prepare staging
rm -rf "${STAGING}"
mkdir -p "${STAGING}/VST3"
mkdir -p "${STAGING}/Components"

# Stage the bundles
cp -R "${BUILD_DIR}/VST3/Giannitune.vst3" "${STAGING}/VST3/"
cp -R "${BUILD_DIR}/AU/Giannitune.component" "${STAGING}/Components/"

echo "  VST3 staged: $(du -sh "${STAGING}/VST3/Giannitune.vst3" | cut -f1)"
echo "  AU   staged: $(du -sh "${STAGING}/Components/Giannitune.component" | cut -f1)"

# Remove quarantine xattrs from staged files (safety)
xattr -rc "${STAGING}" 2>/dev/null || true

# Build two component .pkg files (one per install location)
mkdir -p "${OUT_DIR}"
TMP_PKGS="${OUT_DIR}/_components"
rm -rf "${TMP_PKGS}"
mkdir -p "${TMP_PKGS}"

echo "  Building VST3 component pkg..."
pkgbuild \
    --root "${STAGING}/VST3" \
    --identifier "${IDENTIFIER}.vst3" \
    --version "${VERSION}" \
    --install-location "/Library/Audio/Plug-Ins/VST3" \
    "${TMP_PKGS}/Giannitune-VST3.pkg"

echo "  Building AU component pkg..."
pkgbuild \
    --root "${STAGING}/Components" \
    --identifier "${IDENTIFIER}.au" \
    --version "${VERSION}" \
    --install-location "/Library/Audio/Plug-Ins/Components" \
    "${TMP_PKGS}/Giannitune-AU.pkg"

# Build a distribution.xml for a combined installer
DIST_XML="${TMP_PKGS}/distribution.xml"
cat > "${DIST_XML}" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Giannitune v${VERSION}</title>
    <organization>com.gianni</organization>
    <domains enable_localSystem="true"/>
    <options customize="never" require-scripts="false" rootVolumeOnly="true"/>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <license file="LICENSE" mime-type="text/plain"/>
    <conclusion file="conclusion.txt" mime-type="text/plain"/>

    <choices-outline>
        <line choice="default">
            <line choice="com.gianni.giannitune.vst3"/>
            <line choice="com.gianni.giannitune.au"/>
        </line>
    </choices-outline>

    <choice id="default"/>
    <choice id="com.gianni.giannitune.vst3"
            title="VST3 (Ableton, Reaper, FL Studio, Studio One)"
            visible="true" start_selected="true">
        <pkg-ref id="com.gianni.giannitune.vst3"/>
    </choice>
    <choice id="com.gianni.giannitune.au"
            title="Audio Unit (Logic Pro, GarageBand)"
            visible="true" start_selected="true">
        <pkg-ref id="com.gianni.giannitune.au"/>
    </choice>

    <pkg-ref id="com.gianni.giannitune.vst3"
             version="${VERSION}"
             auth="Root">Giannitune-VST3.pkg</pkg-ref>
    <pkg-ref id="com.gianni.giannitune.au"
             version="${VERSION}"
             auth="Root">Giannitune-AU.pkg</pkg-ref>
</installer-gui-script>
EOF

# Supporting text files for installer pages
cat > "${TMP_PKGS}/welcome.txt" <<EOF
Giannitune v${VERSION}

An open-source vocal pitch-correction plugin.
Installs both VST3 and AU formats to the system plugin folders.

After install, rescan plugins in your DAW.

For Logic Pro users: you may need to clear the audio-unit cache.
See INSTALL.md (included in the installed plugin folder).

Giannitune is licensed under AGPL-3.0 — free and open-source.
EOF

cat > "${TMP_PKGS}/conclusion.txt" <<EOF
Installation complete.

Installed:
  /Library/Audio/Plug-Ins/VST3/Giannitune.vst3
  /Library/Audio/Plug-Ins/Components/Giannitune.component

Next steps:
  1. Open your DAW
  2. Rescan plugins (Logic: clear AudioUnitCache — see INSTALL.md)
  3. Load Giannitune on a vocal track

Report issues: https://github.com/jonnygucci1/Giannitune/issues
Donate:        https://jonnygucci1.gumroad.com/l/giannitune
EOF

cp "${REPO_ROOT}/LICENSE" "${TMP_PKGS}/LICENSE"

echo "  Building final distribution pkg..."
PKG_OUT="${OUT_DIR}/Giannitune-v${VERSION}-macOS.pkg"
productbuild \
    --distribution "${DIST_XML}" \
    --package-path "${TMP_PKGS}" \
    --resources "${TMP_PKGS}" \
    "${PKG_OUT}"

# Cleanup intermediate
rm -rf "${TMP_PKGS}"
rm -rf "${STAGING}"

echo ""
echo "=== DONE ==="
echo "  Output: ${PKG_OUT}"
echo "  Size:   $(du -sh "${PKG_OUT}" | cut -f1)"
echo ""
echo "To test: right-click → Open (unsigned pkg, Gatekeeper will warn)."
