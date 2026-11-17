# syntax=docker/dockerfile:1
#
# Gaspar fork of Sunshine - reproducible Ubuntu 24.04 build.
#
# This Dockerfile only produces release artifacts (.deb + tarball). It is NOT a
# runtime/GPU image: no driver, no encoder validation and no NVIDIA runtime is
# shipped here. GPU/NVENC behaviour must still be validated on real hardware.
#
# Reproducibility pins (bump both together, see docs/sp/reproducibility.md):
#   * Ubuntu 24.04 base image pinned by manifest-list digest, recorded 2026-11-16.
#     manifest list: sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
#     amd64 image:   sha256:77eb101c607d6e3d4f5ad0c8a02c2136c689b98aba1f359e6502ba517e5c57ec
#   * Ubuntu archive snapshot id, recorded 2026-11-16.
#
# BuildKit is required (heredocs, --link, cache mounts).
# Add `--build-arg BUILDKIT_INLINE_CACHE=1` in CI when inline cache metadata is wanted.

# renovate: datasource=docker depName=ubuntu
ARG UBUNTU_BASE=ubuntu:24.04@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
# Snapshot id format: YYYYMMDDTHHMMSSZ. Any timestamp after 2023-03-01 works.
ARG UBUNTU_SNAPSHOT=20261116T000000Z

FROM ${UBUNTU_BASE} AS sunshine-base

ARG UBUNTU_SNAPSHOT
ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV TZ=UTC
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Replace the live archive with the Ubuntu snapshot service. The snapshot hosts
# noble, noble-updates, noble-backports and noble-security for the same id.
# hadolint ignore=DL3008
RUN <<_SNAPSHOT
set -euo pipefail
if [[ ! "${UBUNTU_SNAPSHOT}" =~ ^[0-9]{8}T[0-9]{6}Z$ ]]; then
  echo "Invalid UBUNTU_SNAPSHOT: ${UBUNTU_SNAPSHOT}" >&2
  exit 1
fi

rm -f /etc/apt/sources.list
rm -rf /etc/apt/sources.list.d
mkdir -p /etc/apt/sources.list.d

cat > /etc/apt/sources.list.d/ubuntu.sources <<EOF
Types: deb
URIs: https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}
Suites: noble noble-updates noble-backports noble-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

# Snapshot Release files can carry an old Valid-Until; the snapshot id is the
# integrity boundary, so do not reject packages because of wall-clock time.
printf '%s\n' 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99sp-snapshot

# The ubuntu:24.04 base image ships no CA bundle, so the first HTTPS fetch
# cannot be TLS-verified. Bootstrap ca-certificates from the snapshot: apt still
# verifies the InRelease signature against ubuntu-archive-keyring.gpg, so
# package authenticity is not weakened. Verification is restored right after.
cat > /etc/apt/apt.conf.d/99sp-bootstrap-tls <<'EOF'
Acquire::https::Verify-Peer "false";
Acquire::https::Verify-Host "false";
EOF

apt-get update
apt-get install -y --no-install-recommends ca-certificates
rm -f /etc/apt/apt.conf.d/99sp-bootstrap-tls

# Re-run the index update with TLS verification in place, then leave the image
# clean for the dependency step.
apt-get update
rm -rf /var/lib/apt/lists/*
_SNAPSHOT

# Toolchain + libraries. The upstream script owns the package list so that this
# fork stays aligned with the official Linux .deb build of the tag.
FROM sunshine-base AS sunshine-deps

WORKDIR /build/sunshine/

# Copy only what the dependency step needs, so the layer caches across source
# changes.
COPY --link scripts/linux_build.sh ./scripts/linux_build.sh
COPY --link packaging/linux/patches/ ./packaging/linux/patches/

RUN <<_DEPS
set -e
chmod +x ./scripts/linux_build.sh
./scripts/linux_build.sh \
  --step=deps \
  --sudo-off
apt-get clean
rm -rf /var/lib/apt/lists/*
_DEPS

FROM sunshine-deps AS sunshine-build

ARG SOURCE_DATE_EPOCH=0
ARG BUILD_VERSION=0.0.0
ARG COMMIT=unknown
ARG BRANCH=sp/main
# Full version used in artifact filenames, e.g. 2026.914.233613-sp.0
ARG ARTIFACT_VERSION=0.0.0-sp.0
# Debian release field, e.g. sp.0+ubuntu24.04
ARG DEB_RELEASE=sp.0+ubuntu24.04
ARG TARGETARCH=amd64
ARG BUILDKIT_INLINE_CACHE=1

ENV SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH}
ENV BRANCH=${BRANCH}
ENV BUILD_VERSION=${BUILD_VERSION}
ENV COMMIT=${COMMIT}
ENV DEBIAN_PACKAGE_RELEASE=${DEB_RELEASE}

# The full source tree (including submodules) is required: the FFmpeg build
# dependency, the web UI and the tests all come from it.
COPY --link . .

# Configure, validate, build and package through the upstream script so the
# flags match the official Linux .deb. All Linux capture options stay ON:
# CUDA, DRM, VAAPI, Vulkan, Wayland, X11, KWin and XDG portal.
# BUILD_DOCS is disabled automatically on this image because noble ships
# doxygen 1.9.x, below the script's 1.10 minimum. BUILD_TESTS stays ON.
# hadolint ignore=SC3010,DL4006
RUN <<_BUILD
set -e
./scripts/linux_build.sh \
  --step=cmake \
  --publisher-name='Jaogodela' \
  --publisher-website='https://github.com/Jaogodela/gaspar' \
  --publisher-issue-url='https://github.com/Jaogodela/gaspar/issues' \
  --sudo-off

# Guard the official Linux .deb configuration. These are the capture options
# used by upstream's Debian package; do not turn DRM/VAAPI/Vulkan off.
for option in \
  SUNSHINE_ENABLE_CUDA \
  SUNSHINE_ENABLE_DRM \
  SUNSHINE_ENABLE_VAAPI \
  SUNSHINE_ENABLE_VULKAN \
  SUNSHINE_ENABLE_WAYLAND \
  SUNSHINE_ENABLE_X11 \
  SUNSHINE_ENABLE_KWIN \
  SUNSHINE_ENABLE_PORTAL \
  BUILD_TESTS
do
  grep -q "^${option}:BOOL=ON$" build/CMakeCache.txt || {
    echo "Expected ${option}=ON in build/CMakeCache.txt" >&2
    exit 1
  }
done
# Documentation stays off so CI stays light (doxygen on the pinned snapshot is
# below the script's minimum and the script sets this itself).
grep -q '^BUILD_DOCS:BOOL=OFF$' build/CMakeCache.txt || {
  echo "Expected BUILD_DOCS=OFF in build/CMakeCache.txt" >&2
  exit 1
}

./scripts/linux_build.sh \
  --step=validation \
  --sudo-off

./scripts/linux_build.sh \
  --step=build \
  --sudo-off

./scripts/linux_build.sh \
  --step=package \
  --sudo-off

case "${TARGETARCH}" in
  amd64) tar_arch=x86_64 ;;
  arm64) tar_arch=aarch64 ;;
  *) tar_arch="${TARGETARCH}" ;;
esac

mkdir -p /artifacts
deb_file="$(find build/cpack_artifacts -maxdepth 1 -name '*.deb' -print -quit)"
if [[ -z "${deb_file}" ]]; then
  echo "CPack did not produce a .deb" >&2
  exit 1
fi
cp "${deb_file}" "/artifacts/sunshine_${ARTIFACT_VERSION}_${TARGETARCH}.deb"

# Minimal binary tarball: executable plus the licenses/notices that must travel
# with a GPL-3.0-only redistribution.
tarball_root=/tmp/sunshine-tarball
rm -rf "${tarball_root}"
mkdir -p "${tarball_root}/bin"
install -m 0755 build/sunshine "${tarball_root}/bin/sunshine"
install -m 0644 LICENSE NOTICE "${tarball_root}/"

tar \
  --sort=name \
  --mtime="@${SOURCE_DATE_EPOCH}" \
  --owner=0 \
  --group=0 \
  --numeric-owner \
  -C "${tarball_root}" \
  -czf "/artifacts/sunshine_${ARTIFACT_VERSION}_linux-${tar_arch}.tar.gz" \
  .
rm -rf "${tarball_root}"

# Basic sanity checks on the produced artifacts.
test -s "/artifacts/sunshine_${ARTIFACT_VERSION}_${TARGETARCH}.deb"
test -s "/artifacts/sunshine_${ARTIFACT_VERSION}_linux-${tar_arch}.tar.gz"
tar -tzf "/artifacts/sunshine_${ARTIFACT_VERSION}_linux-${tar_arch}.tar.gz" | grep -qx './bin/sunshine'
_BUILD

# Headless unit tests, mirroring docker/debian.dockerfile. No GPU is present,
# so a passing run says nothing about NVENC/VAAPI/Vulkan encode quality.
FROM sunshine-build AS sunshine-test

WORKDIR /build/sunshine/build/tests

RUN <<_TEST
set -e
export DISPLAY=:1
Xvfb "${DISPLAY}" -screen 0 1024x768x24 &
xvfb_pid=$!
trap 'kill "${xvfb_pid}" 2>/dev/null || true' EXIT
sleep 1
./test_sunshine --gtest_color=yes --gtest_output=xml:/tmp/test_results.xml
_TEST

# Exportable artifact root. Use with:
#   docker buildx build --target artifacts --output type=local,dest=out .
FROM scratch AS artifacts

ARG BUILDKIT_INLINE_CACHE=1
COPY --link --from=sunshine-build /artifacts/ /artifacts/

# Build image published to GHCR. It contains the release artifacts only; it is
# deliberately not a runnable Sunshine image.
FROM artifacts AS ci-image

ARG ARTIFACT_VERSION=0.0.0-sp.0
ARG COMMIT=unknown
LABEL org.opencontainers.image.title="Sunshine (Gaspar fork) build artifacts"
LABEL org.opencontainers.image.description="Ubuntu 24.04 .deb and binary tarball for the Gaspar fork of Sunshine. Not a runtime image."
LABEL org.opencontainers.image.source="https://github.com/Jaogodela/sunshine"
LABEL org.opencontainers.image.url="https://github.com/Jaogodela/gaspar"
LABEL org.opencontainers.image.version="${ARTIFACT_VERSION}"
LABEL org.opencontainers.image.revision="${COMMIT}"
LABEL org.opencontainers.image.licenses="GPL-3.0-only"
