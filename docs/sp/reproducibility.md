# Reproducibility

The Gaspar fork build is pinned so that rebuilding the same commit on the same
architecture produces the same `sunshine` binary.

## Pins

| Pin | Value | Location |
|:--|:--|:--|
| Ubuntu base image | `ubuntu:24.04@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8` (manifest list, recorded 2026-11-16) | `docker/sp-ubuntu-24.04.dockerfile`, `ARG UBUNTU_BASE` |
| Ubuntu archive snapshot | `20261116T000000Z` | `docker/sp-ubuntu-24.04.dockerfile`, `ARG UBUNTU_SNAPSHOT` |
| CUDA toolkit | `13.1.1` / driver build `590.48.01` | defaults in `scripts/linux_build.sh` |
| GCC | `gcc-14` / `g++-14` | selected by `scripts/linux_build.sh` for Ubuntu 24.04 |
| CMake | `4.3.0` when the snapshot version is too old | `scripts/linux_build.sh` |
| Source timestamp | git commit timestamp of the built commit | `SOURCE_DATE_EPOCH` build arg, computed by CI |

The snapshot is the Ubuntu archive as it existed at `2026-11-16T00:00:00Z`. The
snapshot service hosts `noble`, `noble-updates`, `noble-backports` and
`noble-security` at that id, which is why the Dockerfile points every suite at
`https://snapshot.ubuntu.com/ubuntu/<id>` and never at the live archive.

The `ubuntu:24.04` base image does not ship a CA bundle, so the first HTTPS
fetch cannot be TLS-verified. The Dockerfile bootstraps `ca-certificates` from
the snapshot with `Acquire::https::Verify-Peer "false"` for that one command,
then removes the override. apt still checks the snapshot's `InRelease`
signature against `ubuntu-archive-keyring.gpg`, and every subsequent fetch uses
normal TLS verification.

### Bumping a pin

1. Pick a new snapshot id after the base image update, e.g.
   `20270101T000000Z`. Verify the suites resolve:

   ```bash
   for suite in noble noble-updates noble-backports noble-security; do
     curl -fsSI "https://snapshot.ubuntu.com/ubuntu/20270101T000000Z/dists/${suite}/Release"
   done
   ```

2. Update `UBUNTU_SNAPSHOT` and, if needed, the `UBUNTU_BASE` digest.
   The base digest is resolved from the registry manifest list for
   `ubuntu:24.04`; the comment in the Dockerfile records the date it was taken.
3. Update the table above.
4. Run `SP-CI`. The new pins must still produce a reproducible binary.

## SOURCE_DATE_EPOCH

CI computes the git commit timestamp:

```bash
SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
```

It is passed as a build arg, exported into the build stage, and used for:

- compiler date/time macros (`__DATE__`, `__TIME__` are derived from it)
- the tarball `mtime` (`tar --mtime="@${SOURCE_DATE_EPOCH}"`)

The same value must be used for both builds that are compared.

## What CI compares

`SP-CI` builds the commit twice on independent `ubuntu-latest` runners with the
same build args:

1. `build` extracts `bin/sunshine` from the tarball and uploads its sha256.
2. `reproducibility` rebuilds from scratch and compares that sha256.

A mismatch fails the job. The package checksums are printed and compared as a
warning only; the binary is the gated artifact.

Local check:

```bash
# after two local builds into out1 and out2
tar -xzf out1/artifacts/*.tar.gz -C /tmp/r1
tar -xzf out2/artifacts/*.tar.gz -C /tmp/r2
sha256sum /tmp/r1/bin/sunshine /tmp/r2/bin/sunshine
```

## Known non-determinism

These do not affect the `sunshine` ELF binary, which is what CI gates:

- **Web UI assets.** `scripts/linux_build.sh` installs Node.js through nvm
  (`nvm install node`) on Ubuntu 24.04, so the Vite bundle can change between
  runs if a new Node.js release appears. Assets are installed next to the
  binary, not embedded in it, so the binary hash is unaffected, but the `.deb`
  and its checksum can differ.
- **`.deb` container metadata.** CPack/dpkg-deb record file mtimes and control
  metadata that are not fully normalised by `SOURCE_DATE_EPOCH`. The `.deb`
  is therefore not yet gated.
- **Remote downloads.** The NVIDIA CUDA runfile and the Kitware CMake tarball
  are fixed versions fetched from their vendors at build time. Their contents
  are stable, but the HTTP fetch is not snapshot-pinned.
- **FFmpeg prebuilts.** `cmake/dependencies/ffmpeg.cmake` downloads the
  `LizardByte/build-deps` release matching the `third-party/build-deps`
  submodule tag. If that submodule commit is not tagged, it falls back to
  `latest/download`, which is not pinned. Keep the submodule at a tagged commit.
- **Build paths.** Both builds use `/build/sunshine`. Moving the build to a
  different path can leak different absolute paths into the binary (assert
  strings, diagnostics) and break the comparison.

If the binary comparison ever fails unexpectedly, do not relax the check first.
Reproduce locally with two clean builds at the same path, diff the binaries and
identify the source of the difference, then document it here.
