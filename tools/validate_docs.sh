#!/usr/bin/env bash
set -euo pipefail
echo "[validate_docs] Generating API docs"

# Ensure doxygen present
if ! command -v doxygen >/dev/null 2>&1; then
  echo "[validate_docs] ERROR: doxygen not found in PATH." >&2
  echo "Install with (Windows winget): winget install --id Doxygen.Doxygen -e" >&2
  echo "Or (Chocolatey): choco install doxygen.install -y" >&2
  echo "Or (Debian/Ubuntu): sudo apt-get install -y doxygen graphviz" >&2
  exit 127
fi

# Auto-detect Graphviz dot; if missing, create a temp Doxyfile override disabling HAVE_DOT
if ! command -v dot >/dev/null 2>&1; then
  echo "[validate_docs] 'dot' not found – generating docs without graphs (HAVE_DOT=NO)" >&2
  # Copy Doxyfile and flip HAVE_DOT
  sed -E 's/^HAVE_DOT[[:space:]]*=.*/HAVE_DOT               = NO/' Doxyfile > Doxyfile.nodot
  doxygen Doxyfile.nodot 2>&1 | tee docs/api/doxygen.log
  rm -f Doxyfile.nodot
else
  doxygen Doxyfile 2>&1 | tee docs/api/doxygen.log
fi
if grep -i "warning" docs/api/doxygen.log; then
  echo "Doxygen warnings found (treated as error)" >&2
  exit 1
fi
echo "[validate_docs] Packaging documentation artifacts"
art_dir="docs/docx/artifacts"
rm -rf "$art_dir" || true
mkdir -p "$art_dir"
# Copy HTML docs
cp -R docs/api/html "$art_dir/html"
# Copy log
cp docs/api/doxygen.log "$art_dir/"
# Create a lightweight manifest
cat > "$art_dir/manifest.txt" <<MANIFEST
Generated: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
HAVE_DOT: $(grep -E '^HAVE_DOT' Doxyfile | awk '{print $3}') (actual run may differ if fallback applied)
Source Commit: ${GITHUB_SHA:-unknown}
Includes call graphs: $(grep -E '^CALL_GRAPH' Doxyfile | awk '{print $3}')
Files documented: $(find docs/api/html -name '*.html' | wc -l | tr -d ' ')
MANIFEST
# Zip bundle for external consumption (ignore permissions warnings on Windows)
if command -v zip >/dev/null 2>&1; then
  (cd docs/docx && zip -r artifacts.zip artifacts >/dev/null 2>&1)
else
  echo "[validate_docs] 'zip' not found; attempting PowerShell Compress-Archive fallback" >&2
  pwsh -NoLogo -NoProfile -Command "Compress-Archive -Path 'docs/docx/artifacts' -DestinationPath 'docs/docx/artifacts.zip' -Force" 2>/dev/null || true
fi
echo "[validate_docs] Running markdownlint"
markdownlint "**/*.md" --ignore "build" --ignore "docs/api" || exit 1
echo "[validate_docs] Running codespell"
codespell -q 3 || exit 1
echo "[validate_docs] Running link check (lychee)"
lychee --no-progress --accept 200,429 . || exit 1
echo "[validate_docs] SUCCESS"