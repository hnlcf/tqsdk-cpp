#!/bin/sh
# Shared post-install configuration for DEB/RPM packages. It is intentionally
# idempotent and never overwrites an operator-provided config or token.
set -e

if ! getent group tqsdk >/dev/null 2>&1; then
    groupadd --system tqsdk
fi

if ! getent passwd tqsdk >/dev/null 2>&1; then
    NOLOGIN=/sbin/nologin
    [ -x /usr/sbin/nologin ] && NOLOGIN=/usr/sbin/nologin
    useradd --system --gid tqsdk --home-dir /var/lib/tqsdk --shell "$NOLOGIN" tqsdk
fi

install -d -o tqsdk -g tqsdk -m 0750 /var/lib/tqsdk /var/log/tqsdk
install -d -o root -g tqsdk -m 0750 /etc/tqsdk

for name in gw node; do
    if [ -f "/etc/tqsdk/${name}.toml.example" ] && [ ! -e "/etc/tqsdk/${name}.toml" ]; then
        cp "/etc/tqsdk/${name}.toml.example" "/etc/tqsdk/${name}.toml"
        chown root:tqsdk "/etc/tqsdk/${name}.toml"
        chmod 0640 "/etc/tqsdk/${name}.toml"
    fi
done

if [ -f /etc/tqsdk/tqsdk.env.example ] && [ ! -e /etc/tqsdk/tqsdk.env ]; then
    cp /etc/tqsdk/tqsdk.env.example /etc/tqsdk/tqsdk.env
    chown root:root /etc/tqsdk/tqsdk.env
    chmod 0600 /etc/tqsdk/tqsdk.env
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

cat <<'EOF'
tqsdk: installed. Edit /etc/tqsdk/gw.toml and /etc/tqsdk/node.toml,
       set TQSDK_SERVE_TOKEN in /etc/tqsdk/tqsdk.env, then:
       systemctl enable --now tqsdk.target
EOF
exit 0
