# SPDX-License-Identifier: Apache-2.0
# The rayopdf container image: the fully static linux-x64 binary on an empty base, run as an
# unprivileged user in /w. The build context is an extracted core archive
# (rayopdf-VERSION-linux-x64-static/, tools/package.py):
#
#   docker build -f packaging/container/Dockerfile --build-arg VERSION=$(cat VERSION) \
#       -t rayopdf rayopdf-$(cat VERSION)-linux-x64-static
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/w" rayopdf --convert in.pdf out.md
#
# The Release workflow builds it from the release's archive and pushes it to ghcr.io on a tag.

# The folders the binary needs: /tmp (sticky, world-writable) and the working folder /w, owned by
# the user the image runs as. Only these folders come from Debian.
FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc AS rootfs
RUN mkdir -p /rootfs/tmp /rootfs/w && chmod 1777 /rootfs/tmp && chown 65532:65532 /rootfs/w

FROM scratch
ARG VERSION
LABEL org.opencontainers.image.title="rayopdf" \
      org.opencontainers.image.description="PDF to Markdown from a single static executable" \
      org.opencontainers.image.source="https://github.com/Butterski/rayopdf" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.version="${VERSION}"
COPY --from=rootfs /rootfs/ /
COPY LICENSE NOTICE /usr/share/doc/rayopdf/
COPY third_party/ /usr/share/doc/rayopdf/third_party/
COPY --chmod=0755 rayopdf /rayopdf
USER 65532:65532
WORKDIR /w
ENTRYPOINT ["/rayopdf"]
