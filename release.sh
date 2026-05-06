#!/bin/bash
set -e

CURRENT=$(cat VERSION 2>/dev/null | tr -d '\n')
PREV_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "none")
echo "Current version: ${CURRENT:-unknown}"
echo "Latest git tag:  $PREV_TAG"
echo ""
read -rp "New version (e.g. 1.0.0): " VERSION

if [[ -z "$VERSION" ]]; then
    echo "No version entered. Aborting."
    exit 1
fi

TAG="v$VERSION"

# Check remote tags without fetching locally
if git ls-remote --tags origin | grep -q "refs/tags/$TAG$"; then
    echo "Tag $TAG already exists on remote. Aborting."
    exit 1
fi

# Update VERSION file
echo "$TAG" > VERSION
echo "Updated VERSION to $TAG"

# Build firmware
echo "Building firmware..."
~/.platformio/penv/bin/pio run -e heltec-v4-tft
echo "Build successful."

# Commit and push all changes
git add -A
git commit -m "Release $TAG"
git push

echo "Changes committed and pushed."

# Remove stale local tag if present (not on remote, so safe to recreate)
if git tag | grep -q "^$TAG$"; then
    git tag -d "$TAG"
fi

git tag "$TAG"
git push origin "$TAG"

if command -v gh >/dev/null 2>&1; then
    if gh release view "$TAG" >/dev/null 2>&1; then
        echo "GitHub release $TAG already exists."
        echo "If it is still a draft, publish it to trigger the GitHub Actions build."
    else
        gh release create "$TAG" --generate-notes --title "$TAG"
        echo "GitHub release published for $TAG."
    fi

    echo "Release workflow should start from the published release."
else
    echo "Tag $TAG pushed."
    echo "Create and publish a GitHub release for $TAG to trigger the GitHub Actions build."
fi

echo "https://github.com/oumike/daisy-aprs/releases"
