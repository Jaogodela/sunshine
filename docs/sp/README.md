# Sunshine (Gaspar fork) Linux builds

This directory documents the fork-specific build and release tooling. The
upstream README, workflows and build scripts are intentionally left alone so the
fork keeps rebasing cleanly onto upstream Sunshine tags.

- Fork: <https://github.com/Jaogodela/sunshine>
- Product monorepo: <https://github.com/Jaogodela/gaspar>
- Upstream: SunshineStream/Sunshine, GPL-3.0-only

Everything here builds an Ubuntu 24.04 `amd64` `.deb` plus a binary tarball.
There is no GPU in the build environment: these artifacts are **not** proof that
NVENC, VAAPI or Vulkan encoding works. That validation stays manual.

## Branch model

| Branch | Purpose | Rules |
|:--|:--|:--|
| `sp/main` | Integration branch of the fork. | CI (`SP-CI`) runs here. PRs land here. |
| `upstream-sync` | Mirror of `upstream/master`. | Never commit on it. Only fast-forward from the upstream remote. |
| `sp/feat/*` | Topic branches. | Branch from `sp/main`, PR back into `sp/main`. |

The `upstream` remote has a deliberately disabled push URL. Never push to
`SunshineStream/Sunshine` or open PRs against it.

## Local build

Requirements: Docker with BuildKit/buildx, `git`, and a checkout with
submodules.

```bash
git clone --recurse-submodules https://github.com/Jaogodela/sunshine.git
cd sunshine
git checkout sp/main

SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
docker buildx build \
  --file docker/sp-ubuntu-24.04.dockerfile \
  --target artifacts \
  --output type=local,dest=out \
  --build-arg SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH}" \
  --build-arg BUILD_VERSION=2026.914.233613 \
  --build-arg ARTIFACT_VERSION=2026.914.233613-sp.0 \
  --build-arg DEB_RELEASE=sp.0+ubuntu24.04 \
  --build-arg COMMIT="$(git rev-parse HEAD)" \
  --build-arg BRANCH=sp/main \
  .
```

Results:

```
out/artifacts/sunshine_2026.914.233613-sp.0_amd64.deb
out/artifacts/sunshine_2026.914.233613-sp.0_linux-x86_64.tar.gz
```

The tarball contains `bin/sunshine`, `LICENSE` and `NOTICE`. It does not carry
the runtime assets; install the `.deb` for a normal system installation.

Run the headless unit tests (Xvfb, no GPU):

```bash
docker buildx build \
  --file docker/sp-ubuntu-24.04.dockerfile \
  --target sunshine-test \
  --build-arg SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH}" \
  --build-arg BUILD_VERSION=2026.914.233613 \
  --build-arg ARTIFACT_VERSION=2026.914.233613-sp.0 \
  --build-arg DEB_RELEASE=sp.0+ubuntu24.04 \
  --build-arg COMMIT="$(git rev-parse HEAD)" \
  .
```

Install and inspect the package:

```bash
sudo apt install ./out/artifacts/sunshine_*_amd64.deb
dpkg-deb --info out/artifacts/sunshine_*_amd64.deb
sunshine --version
```

### Build configuration

The Dockerfile calls `scripts/linux_build.sh` rather than duplicating upstream
flags. That keeps the official Linux capture options enabled:

- CUDA, DRM/KMS, VA-API, Vulkan video
- Wayland, X11, KWin ScreenCast, XDG portal

`BUILD_TESTS=ON`. Documentation is skipped automatically because the pinned
snapshot ships doxygen 1.9.x, below the script's 1.10 minimum.

Publisher metadata compiled into the binary identifies the fork (Jaogodela,
<https://github.com/Jaogodela/gaspar>, issue tracker
<https://github.com/Jaogodela/gaspar/issues>).

## CI

`.github/workflows/sp-ci.yml` runs on pull requests and on pushes to `sp/main`
and `sp/**`. It does not run on upstream branches.

1. `prepare` derives the base version from the nearest `v*` tag, and reads
   `SOURCE_DATE_EPOCH` from the commit timestamp.
2. `build` compiles and runs the unit tests under Xvfb, extracts the `.deb` and
   tarball, and uploads `sp-deb` and `sp-checksums`.
3. `reproducibility` rebuilds the same commit on a fresh runner with the same
   `SOURCE_DATE_EPOCH` and fails if the `bin/sunshine` sha256 differs from the
   first build.

Both jobs use `ubuntu-latest` (amd64) and a 180-minute timeout. The build runs
without a GPU; a green run is not GPU validation.

## Release

`.github/workflows/sp-release.yml` triggers on tags matching `v*-sp.*` (for
example `v2026.914.233613-sp.0`) and on manual dispatch.

It builds the same artifacts, publishes the artifact image to
`ghcr.io/jaogodela/sunshine:<version>`, signs it keyless with cosign, and
attaches to the GitHub Release:

- `.deb`
- `linux-x86_64` tarball
- `SHA256SUMS`
- `SBOM.spdx.json` (syft)

Release creation is idempotent: an existing release gets its assets replaced.

### First-time fork setup

Before the first release:

1. Enable GitHub Actions on the fork.
2. Let Actions create packages; the default `GITHUB_TOKEN` needs
   `packages: write` (already requested by the workflow).
3. The first pushed package is private. Open the package settings for
   `jaogodela/sunshine`, link it to this repository, and set visibility as
   desired. Subsequent pushes keep working without manual login.
4. Optional: protect `sp/main` and require `SP-CI` to be green.

Tag only after `SP-CI` is green on the commit that will be released:

```bash
git checkout sp/main
git pull --ff-only
git tag v2026.914.233613-sp.0
git push origin v2026.914.233613-sp.0
```

## Known limitations

- No GPU/NVENC/VAAPI validation in CI. Do that on the owner's WSL machine with
  a real NVIDIA GPU before calling a build playable.
- Only `amd64` is produced. The Dockerfile carries a `TARGETARCH` argument for
  filenames, but no arm64 matrix is wired up.
- The `.deb` is not yet byte-identical across rebuilds; the binary is gated.
  See [reproducibility.md](reproducibility.md).
- The AppStream metadata and CPack vendor/maintainer fields are still the
  upstream LizardByte values because the fork does not patch upstream packaging
  templates. The publisher metadata compiled into the binary is the fork's
  (Jaogodela).

## Upstream sync

`.github/workflows/upstream-sync.yml` runs every Monday at 06:00 UTC and on
manual dispatch. It fast-forwards the `upstream-sync` branch to
`upstream/master` and, when a new stable tag appears, rebases the commits on
`sp/main` onto that tag, then opens a PR (or an issue, if the rebase
conflicts). It never pushes to upstream. A live stream smoke test after the PR
is still manual.
