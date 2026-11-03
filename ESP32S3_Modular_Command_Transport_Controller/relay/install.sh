#!/usr/bin/env bash
# Ubuntu server installer. Source this file to test helpers without running setup.
set -euo pipefail
valid_domain(){ [[ ${#1} -le 253 && $1 == *.* && $1 =~ ^[a-zA-Z0-9]([a-zA-Z0-9.-]*[a-zA-Z0-9])?$ && $1 != *..* ]] &&
  awk -F. '{for(i=1;i<=NF;i++)if(length($i)>63||$i~/^-|-$/)exit 1}' <<< "$1"; }
install_prerequisites(){
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-remove ca-certificates curl openssl python3 tar
}
install_docker(){
  local apt_root=${1:-/etc/apt}
  if command -v docker >/dev/null && docker compose version >/dev/null 2>&1; then
    echo 'Reusing installed Docker Engine and Compose.'
  else
    install -m 0755 -d "$apt_root/keyrings" "$apt_root/sources.list.d"
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o "$apt_root/keyrings/docker.asc"
    chmod a+r "$apt_root/keyrings/docker.asc"
    printf 'Types: deb\nURIs: https://download.docker.com/linux/ubuntu\nSuites: %s\nComponents: stable\nArchitectures: %s\nSigned-By: %s/keyrings/docker.asc\n' \
      "$VERSION_CODENAME" "$(dpkg --print-architecture)" "$apt_root" > "$apt_root/sources.list.d/docker.sources"
    apt-get update
    if command -v docker >/dev/null; then
      # --no-remove refuses to replace an incompatible existing engine.
      apt-get install -y --no-remove --no-install-recommends docker-compose-plugin
    else
      apt-get install -y --no-remove docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
    fi
  fi
  if systemctl cat docker.service >/dev/null 2>&1;then systemctl enable --now docker;fi
  docker info >/dev/null
  docker compose version
}
configure_relay(){
  local web=$1 mqtt=$2 device=$3 variant=$4
  if [[ -e .env || -e secrets ]]; then
    [[ -f .env && -f secrets/device-settings.txt && -f secrets/ca.crt ]] || {
      echo 'Incomplete previous configuration. Restore its complete .env/secrets set; nothing was overwritten.' >&2;return 1; }
    python3 - "$web" "$mqtt" "$device" <<'PY'
import sys
from pathlib import Path
values=dict(line.split('=',1) for line in Path('.env').read_text().splitlines() if '=' in line and not line.startswith('#'))
for key,expected in zip(('WEB_DOMAIN','MQTT_DOMAIN','DEVICE_ID'),sys.argv[1:]):
    if values.get(key,'').strip("'\"")!=expected:sys.exit('Existing deployment has different names/device ID. Use its original settings; credentials were preserved.')
PY
    echo 'Reusing existing credentials and CA.'
  else
    sh ./setup.sh "$web" "$mqtt" "$device"
  fi
  python3 - "$variant" <<'PY'
import sys
from pathlib import Path
path=Path('.env');variant=sys.argv[1]
source='../../ESP32S3_Dispenser_Controller/web' if variant=='s3-dispenser' else '../web'
lines=[line for line in path.read_text().splitlines() if not line.startswith(('WEB_SOURCE=','FIRMWARE_VARIANT='))]
path.write_text('\n'.join(lines+['WEB_SOURCE='+source,'FIRMWARE_VARIANT='+variant])+'\n');path.chmod(0o600)
PY
  python3 ./export_settings.py .
  # The broker does not need the CA signing key in its mounted secrets folder.
  if [[ -f secrets/ca.key && ! -e backups/ca.key ]]; then
    install -m 0700 -d backups
    mv -- secrets/ca.key backups/ca.key
    chmod 600 backups/ca.key
  fi
}
start_relay(){
  docker compose config --quiet
  docker compose up -d --build
  local ready=0
  for ((i=0;i<30;i++));do
    if docker compose exec -T relay node --input-type=module -e 'import {checkHealth} from "./healthcheck.mjs";const h=await checkHealth();if(!h.broker.connected)process.exit(1)' >/dev/null 2>&1;then ready=1;break;fi
    sleep 2
  done
  [[ $ready == 1 ]] || { docker compose ps;echo 'Relay HTTP did not become ready. Run: sudo docker compose logs --tail=80' >&2;return 1; }
}
main(){
  local web='' mqtt='' device='s3-controller' variant='s3-dispenser' dry=0
  while (($#));do
    case "$1" in
      --web-domain|--mqtt-domain|--device-id|--variant) [[ $# -ge 2 ]] || { echo 'Missing option value' >&2;return 2; }
        case "$1" in --web-domain) web=$2;;--mqtt-domain)mqtt=$2;;--device-id)device=$2;;--variant)variant=$2;;esac;shift 2;;
      --dry-run)dry=1;shift;;*)echo "Unknown option: $1" >&2;return 2;;
    esac
  done
  valid_domain "$web" && valid_domain "$mqtt" || { echo 'Supply valid --web-domain and --mqtt-domain DNS names.' >&2;return 2; }
  [[ $device =~ ^[a-zA-Z0-9_-]{1,40}$ && $device != relay ]] || { echo 'Invalid device ID' >&2;return 2; }
  [[ $variant == s3-dispenser || $variant == s3-transport ]] || { echo 'Select an S3 variant' >&2;return 2; }
  if ((dry));then
    printf 'DRY RUN: install Ubuntu prerequisites; install/reuse Docker Engine + Compose; preserve/generate credentials; select %s UI; build/start relay; export private pairing JSON.\n' "$variant"
    printf 'DNS: %s and %s must point directly to this VPS. Open inbound TCP 80,443,8883 in its cloud firewall.\n' "$web" "$mqtt"
    return
  fi
  [[ $(id -u) == 0 ]] || { echo 'Run with sudo (or as root).' >&2;return 1; }
  # shellcheck disable=SC1091
  source /etc/os-release
  [[ $ID == ubuntu && $VERSION_ID =~ ^(22\.04|24\.04|26\.04|26\.10)$ ]] || { echo 'Use a supported Ubuntu VPS (22.04/24.04/26.04/26.10).' >&2;return 1; }
  cd -- "$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")"
  for domain in "$web" "$mqtt";do getent ahosts "$domain" >/dev/null || { echo "DNS not ready: $domain. Point it to this VPS and rerun." >&2;return 1; };done
  install_prerequisites
  install_docker
  configure_relay "$web" "$mqtt" "$device" "$variant"
  start_relay
  printf '\nRelay and broker ready. Console: https://%s\nPrivate pairing file: %s/secrets/device-settings.json\n' "$web" "$PWD"
  printf 'Cloud firewall needs TCP 80,443,8883. Existing firewall rules and other services were not changed.\n'
  if [[ $(curl -sS --connect-timeout 5 --max-time 10 -o /dev/null -w '%{http_code}' "https://$web/" || true) != 401 ]];then
    echo 'Public HTTPS is still pending. Check DNS/cloud firewall and docker compose logs; server credentials were saved.'
  fi
}
if [[ ${BASH_SOURCE[0]} == "$0" ]];then main "$@";fi
