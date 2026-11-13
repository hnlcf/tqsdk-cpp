# Runtime image for the release tree produced by scripts/package_release.sh.
# Build from the repository root:
#   podman build -f deploy/container/Dockerfile -t tqsdk:0.1.0 .
ARG BASE_IMAGE=registry.fedoraproject.org/fedora:44
FROM ${BASE_IMAGE}

ARG TQSDK_VERSION=0.4.0
ARG RELEASE_DIR=dist/stage/tqsdk-0.1.0

LABEL org.opencontainers.image.title="TQSDK Board"
LABEL org.opencontainers.image.description="TQSDK gateway/node and SolidJS web terminal"
LABEL org.opencontainers.image.version="${TQSDK_VERSION}"

RUN dnf install -y --setopt=install_weak_deps=False \
        nginx \
        curl \
        ca-certificates \
        tini \
        shadow-utils \
    && dnf clean all \
    && rm -rf /var/cache/dnf

RUN useradd --system --home-dir /var/lib/tqsdk --shell /sbin/nologin tqsdk \
    && install -d -o tqsdk -g tqsdk -m 0750 /var/lib/tqsdk /var/log/tqsdk \
    && install -d -o root -g root -m 0755 /opt/tqsdk/bin /opt/tqsdk/lib /opt/tqsdk/web \
    && install -d -o root -g root -m 0755 /etc/tqsdk

# The release staging tree is created by scripts/package_release.sh.
COPY ${RELEASE_DIR}/usr/bin/ /opt/tqsdk/bin/
COPY ${RELEASE_DIR}/usr/lib/ /opt/tqsdk/lib/
COPY ${RELEASE_DIR}/usr/share/tqsdk/web/ /opt/tqsdk/web/
COPY deploy/container/nginx.conf /etc/nginx/nginx.conf
COPY deploy/container/node-sim.toml /etc/tqsdk/node-sim.toml
COPY deploy/container/node-live.toml /etc/tqsdk/node-live.toml
COPY deploy/container/gw-sim.toml /etc/tqsdk/gw-sim.toml
COPY deploy/container/entrypoint.sh /opt/tqsdk/entrypoint.sh
RUN chmod 0755 /opt/tqsdk/entrypoint.sh \
    && chmod 0755 /opt/tqsdk/bin/tqsdk-gw /opt/tqsdk/bin/tqsdk-node \
    && ln -sf ../lib /opt/tqsdk/lib64

ENV PATH="/opt/tqsdk/bin:${PATH}" \
    LD_LIBRARY_PATH="/opt/tqsdk/lib" \
    TQSDK_ROLE=node \
    TQSDK_NODE_CONFIG=/etc/tqsdk/node-sim.toml \
    TQSDK_GW_CONFIG=/etc/tqsdk/gw-sim.toml

EXPOSE 8080
VOLUME ["/var/lib/tqsdk", "/var/log/tqsdk"]

HEALTHCHECK --interval=5s --timeout=2s --start-period=5s --retries=12 \
    CMD curl -fsS http://127.0.0.1:8080/healthz >/dev/null || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/opt/tqsdk/entrypoint.sh"]
