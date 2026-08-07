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

# The base image's HEALTHCHECK curls ${AQUALINK_API_URL}/api/health, and the base
# default points at the app's default port 80 — but run.sh starts the app on the
# add-on's fixed port 8099. Without this override the probe fails forever, Docker
# reports the container "unhealthy", the Supervisor never shows the add-on as
# "Running", and the add-on Watchdog kill/restarts it every ~80s. (The Matter
# sidecar, the env's other consumer, is disabled in the add-on.)
ENV AQUALINK_API_URL=http://127.0.0.1:8099

# tini (from the base image) stays PID 1 and now supervises run.sh, which builds the
# argv and execs the base image's docker-entrypoint.sh.
ENTRYPOINT ["/usr/bin/tini", "--", "/run.sh"]
CMD []
