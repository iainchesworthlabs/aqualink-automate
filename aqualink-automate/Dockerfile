# Home Assistant add-on image. Thin layer over the published app image: add bashio
# (HA's options/services helper) + run.sh, and re-point the entrypoint at run.sh so
# it translates the add-on options form into the app's CLI flags at start.
#
# The base is the published multi-arch app image at THIS add-on's version. The
# Supervisor provides BUILD_VERSION (= config.yaml `version`) on a local build; CI and
# the dev harness pass it explicitly. (`build.yaml`/`BUILD_FROM` auto-injection was
# removed in Supervisor 2026.04.) Docker resolves the correct arch from the manifest.
# The base already ships tini (PID 1), curl, and docker-entrypoint.sh; we add jq + bashio.
ARG BUILD_VERSION
FROM ghcr.io/iainchesworth/aqualink-automate:${BUILD_VERSION}

LABEL org.opencontainers.image.title="Aqualink Automate" \
      org.opencontainers.image.source="https://github.com/iainchesworth/aqualink-automate"

# bashio is a distro-agnostic bash library, so it runs fine on the app's Ubuntu
# (glibc) base — no musl/Alpine assumptions. jq is bashio's only runtime dep.
ARG BASHIO_VERSION=v0.16.2
RUN apt-get update \
    && apt-get install -y --no-install-recommends jq \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /tmp/bashio \
    && curl -L -f -s "https://github.com/hassio-addons/bashio/archive/refs/tags/${BASHIO_VERSION}.tar.gz" \
        | tar -xzf - --strip-components=1 -C /tmp/bashio \
    && mv /tmp/bashio/lib /usr/lib/bashio \
    && ln -s /usr/lib/bashio/bashio /usr/bin/bashio \
    && rm -rf /tmp/bashio

COPY run.sh /run.sh
RUN chmod a+x /run.sh

# tini (from the base image) stays PID 1 and now supervises run.sh, which builds the
# argv and execs the base image's docker-entrypoint.sh.
ENTRYPOINT ["/usr/bin/tini", "--", "/run.sh"]
CMD []
