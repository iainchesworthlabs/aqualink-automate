# Home Assistant add-on image. Thin layer over the published app image: add bashio
# (HA's options/services helper) + run.sh, and re-point the entrypoint at run.sh so
# it translates the add-on options form into the app's CLI flags at start.
#
# BUILD_FROM is supplied by the Supervisor from build.yaml (the published multi-arch
# app image). The base image already ships tini (PID 1), curl, and the app
# entrypoint (docker-entrypoint.sh); we only add jq + bashio here.
ARG BUILD_FROM
FROM ${BUILD_FROM}

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
