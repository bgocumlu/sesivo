#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C

readonly REPOSITORY="bgocumlu/sesivo"
readonly SERVICE_NAME="sesivo-server"
readonly INSTALL_PATH="/usr/local/bin/sesivo-server"
readonly CONFIG_DIR="/etc/sesivo"
readonly CONFIG_PATH="${CONFIG_DIR}/server.env"
readonly UNIT_PATH="/etc/systemd/system/${SERVICE_NAME}.service"

requested_version="${SESIVO_VERSION:-}"
requested_port=""
requested_server_id=""
work_dir=""

usage() {
    cat <<'EOF'
Install or update the Sesivo headless server on a systemd-based Linux host.

Usage:
  sudo bash install-server.sh [options]

Options:
  --version <vX.Y.Z>   Install a specific release (default: latest)
  --port <port>        Set the UDP listen port (default on first install: 9999)
  --server-id <id>     Set a stable server ID using letters, numbers, '.', '_', '-'
  -h, --help           Show this help

Re-running the installer updates the binary and preserves existing configuration
unless --port or --server-id is explicitly supplied.
EOF
}

die() {
    printf 'install-server: %s\n' "$*" >&2
    exit 1
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

is_valid_version() {
    [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

is_valid_port() {
    [[ "$1" =~ ^[0-9]{1,5}$ ]] && (( 10#$1 >= 1 && 10#$1 <= 65535 ))
}

is_valid_server_id() {
    [[ "$1" =~ ^[A-Za-z0-9._-]{1,63}$ ]]
}

download() {
    local url="$1"
    local output="$2"
    curl --fail --silent --show-error --location \
        --retry 3 --retry-all-errors --connect-timeout 10 \
        --output "$output" "$url"
}

resolve_latest_version() {
    local final_url
    final_url="$(
        curl --fail --silent --show-error --location \
            --retry 3 --retry-all-errors --connect-timeout 10 \
            --output /dev/null --write-out '%{url_effective}' \
            "https://github.com/${REPOSITORY}/releases/latest"
    )"
    printf '%s\n' "${final_url##*/}"
}

default_server_id() {
    local host
    host="$(hostname -s 2>/dev/null || hostname 2>/dev/null || true)"
    host="$(printf '%s' "$host" | tr -cd 'A-Za-z0-9._-' | cut -c1-56)"
    if [[ -n "$host" ]]; then
        printf '%s-sesivo\n' "$host"
    else
        printf 'self-hosted\n'
    fi
}

read_config_value() {
    local key="$1"
    if [[ -f "$CONFIG_PATH" ]]; then
        sed -n "s/^${key}=//p" "$CONFIG_PATH" | tail -n 1
    fi
}

set_config_value() {
    local key="$1"
    local value="$2"
    if grep -q "^${key}=" "$CONFIG_PATH" 2>/dev/null; then
        sed -i "s|^${key}=.*|${key}=${value}|" "$CONFIG_PATH"
    else
        printf '%s=%s\n' "$key" "$value" >> "$CONFIG_PATH"
    fi
}

cleanup() {
    if [[ -n "$work_dir" && -d "$work_dir" ]]; then
        rm -rf -- "$work_dir"
    fi
}
trap cleanup EXIT

while (( $# > 0 )); do
    case "$1" in
        --version)
            (( $# >= 2 )) || die "--version requires a value"
            requested_version="$2"
            shift 2
            ;;
        --port)
            (( $# >= 2 )) || die "--port requires a value"
            requested_port="$2"
            shift 2
            ;;
        --server-id)
            (( $# >= 2 )) || die "--server-id requires a value"
            requested_server_id="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

if [[ -n "$requested_version" ]]; then
    is_valid_version "$requested_version" || \
        die "release version must use vX.Y.Z format: $requested_version"
fi
if [[ -n "$requested_port" ]]; then
    is_valid_port "$requested_port" || die "invalid UDP port: $requested_port"
fi
if [[ -n "$requested_server_id" ]]; then
    is_valid_server_id "$requested_server_id" || \
        die "server ID must contain 1-63 letters, numbers, '.', '_', or '-'"
fi

[[ "$(uname -s)" == "Linux" ]] || die "this installer supports Linux only"
(( EUID == 0 )) || die "run this installer as root (for example: sudo bash install-server.sh)"
[[ -d /run/systemd/system ]] || die "systemd is not running on this host"

for command_name in cat chmod chown curl cut getent grep groupadd hostname id \
                    install mkdir mktemp mv rm sed sha256sum systemctl tail tar \
                    touch tr uname useradd; do
    need_command "$command_name"
done

case "$(uname -m)" in
    x86_64)
        architecture="x64"
        ;;
    aarch64|arm64)
        architecture="arm64"
        ;;
    *)
        die "unsupported architecture: $(uname -m) (supported: x86_64, aarch64)"
        ;;
esac

if [[ -z "$requested_version" ]]; then
    requested_version="$(resolve_latest_version)"
fi
is_valid_version "$requested_version" || \
    die "release version must use vX.Y.Z format: $requested_version"

archive="sesivo-server-${requested_version}-linux-${architecture}.tar.gz"
release_url="https://github.com/${REPOSITORY}/releases/download/${requested_version}"
work_dir="$(mktemp -d)"

printf 'Downloading Sesivo Server %s for Linux %s...\n' \
    "$requested_version" "$architecture"
download "${release_url}/${archive}" "${work_dir}/${archive}"
download "${release_url}/${archive}.sha256" "${work_dir}/${archive}.sha256"

(
    cd "$work_dir"
    sha256sum --check --status "${archive}.sha256"
) || die "checksum verification failed for $archive"
printf 'Checksum verified.\n'

mkdir -p "${work_dir}/unpacked"
tar -xzf "${work_dir}/${archive}" -C "${work_dir}/unpacked"
binary="${work_dir}/unpacked/sesivo-server"
[[ -f "$binary" && ! -L "$binary" ]] || die "release archive contains no server binary"

if ! getent group sesivo >/dev/null; then
    groupadd --system sesivo
fi
if ! id -u sesivo >/dev/null 2>&1; then
    useradd --system --gid sesivo --home-dir /var/lib/sesivo \
        --no-create-home --shell /usr/sbin/nologin sesivo
fi

install -d -m 0755 "$CONFIG_DIR"
install -d -o sesivo -g sesivo -m 0750 /var/lib/sesivo /var/log/sesivo

existing_port="$(read_config_value SESIVO_PORT)"
existing_server_id="$(read_config_value SESIVO_SERVER_ID)"

if [[ -n "$requested_port" ]]; then
    configured_port="$requested_port"
elif is_valid_port "$existing_port"; then
    configured_port="$existing_port"
else
    configured_port="9999"
fi

if [[ -n "$requested_server_id" ]]; then
    configured_server_id="$requested_server_id"
elif is_valid_server_id "$existing_server_id"; then
    configured_server_id="$existing_server_id"
else
    configured_server_id="$(default_server_id)"
fi

touch "$CONFIG_PATH"
set_config_value SESIVO_PORT "$configured_port"
set_config_value SESIVO_SERVER_ID "$configured_server_id"
chown root:sesivo "$CONFIG_PATH"
chmod 0640 "$CONFIG_PATH"

install -m 0755 "$binary" "${INSTALL_PATH}.new"
mv -f "${INSTALL_PATH}.new" "$INSTALL_PATH"

install -d -m 0755 /usr/local/share/doc/sesivo-server
install -m 0644 "${work_dir}/unpacked/LICENSE" \
    "${work_dir}/unpacked/THIRD_PARTY_NOTICES.md" \
    /usr/local/share/doc/sesivo-server/

cat > "$UNIT_PATH" <<'UNIT'
[Unit]
Description=Sesivo low-latency audio server
Documentation=https://github.com/bgocumlu/sesivo
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=sesivo
Group=sesivo
EnvironmentFile=/etc/sesivo/server.env
WorkingDirectory=/var/lib/sesivo
StateDirectory=sesivo
StateDirectoryMode=0750
LogsDirectory=sesivo
LogsDirectoryMode=0750
ExecStart=/usr/local/bin/sesivo-server --port ${SESIVO_PORT} --server-id ${SESIVO_SERVER_ID} --log-file /var/log/sesivo/server.log --crash-report-dir /var/lib/sesivo/crash_reports
Restart=on-failure
RestartSec=3
UMask=0027
LimitNOFILE=65536

NoNewPrivileges=true
PrivateDevices=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictSUIDSGID=true
LockPersonality=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
CapabilityBoundingSet=

[Install]
WantedBy=multi-user.target
UNIT

chmod 0644 "$UNIT_PATH"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME" >/dev/null
if ! systemctl restart "$SERVICE_NAME"; then
    systemctl status "$SERVICE_NAME" --no-pager || true
    command -v journalctl >/dev/null 2>&1 && \
        journalctl -u "$SERVICE_NAME" -n 50 --no-pager || true
    die "the service failed to start"
fi

printf '\nSesivo Server %s is installed and running.\n' "$requested_version"
printf 'Address: <this server public IP or hostname>:%s (UDP)\n' "$configured_port"
printf '\nManage it with:\n'
printf '  sudo systemctl status %s\n' "$SERVICE_NAME"
printf '  sudo journalctl -u %s -f\n' "$SERVICE_NAME"
printf '\nConfiguration: %s\n' "$CONFIG_PATH"
printf 'After changing it, run: sudo systemctl restart %s\n' "$SERVICE_NAME"
printf '\nRemember to allow UDP %s in the host/cloud firewall.\n' "$configured_port"
printf 'Home hosts must also forward UDP %s in their router.\n' "$configured_port"
