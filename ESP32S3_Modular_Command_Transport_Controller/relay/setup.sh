#!/bin/sh
# Run once on the Linux host, from this directory. Requires Docker + OpenSSL + Python 3.
set -eu
umask 077
[ "$#" -ge 2 ] || { echo "Usage: sh setup.sh control.your-domain.com mqtt.your-domain.com [device-id]"; exit 1; }
web_domain=$1
mqtt_domain=$2
device_id=${3:-s3-controller}
[ -n "$web_domain" ] && [ -n "$mqtt_domain" ] || { echo 'Both DNS names are required'; exit 1; }
case "$web_domain$mqtt_domain" in *[!a-zA-Z0-9.-]*) echo 'Use DNS hostnames only'; exit 1;; esac
case "$device_id" in relay|''|*[!a-zA-Z0-9_-]*) echo 'Invalid device ID'; exit 1;; esac
[ "${#device_id}" -le 40 ] || { echo 'Device ID is too long'; exit 1; }
[ ! -e .env ] && [ ! -e secrets ] || { echo 'Existing configuration found; setup never overwrites it'; exit 1; }
base=$PWD
work=$(mktemp -d "$base/.relay-setup-XXXXXXXX")
cleanup() { case "$work" in "$base"/.relay-setup-*) [ ! -d "$work" ] || rm -rf -- "$work";; esac; }
trap cleanup EXIT HUP INT TERM
cd "$work"
mkdir secrets
openssl genrsa -out secrets/ca.key 2048
openssl req -x509 -new -sha256 -days 3650 -key secrets/ca.key -out secrets/ca.crt -subj '/CN=Field relay CA' -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign'
openssl genrsa -out secrets/broker.key 2048
openssl req -new -sha256 -key secrets/broker.key -out secrets/broker.csr -subj "/CN=$mqtt_domain"
printf 'subjectAltName=DNS:%s,DNS:broker\nbasicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' "$mqtt_domain" > secrets/extensions.cnf
openssl x509 -req -sha256 -days 365 -in secrets/broker.csr -CA secrets/ca.crt -CAkey secrets/ca.key -CAcreateserial -extfile secrets/extensions.cnf -out secrets/broker.crt
device_password=$(openssl rand -hex 24)
relay_password=$(openssl rand -hex 24)
internal_key=$(openssl rand -hex 32)
web_password=$(openssl rand -hex 12)
web_hash=$(docker run --rm caddy:2-alpine caddy hash-password --plaintext "$web_password")
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/secrets:/work" eclipse-mosquitto:2 mosquitto_passwd -b -c /work/passwords "$device_id" "$device_password"
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/secrets:/work" eclipse-mosquitto:2 mosquitto_passwd -b /work/passwords relay "$relay_password"
cat > secrets/acl <<EOF
user $device_id
topic read devices/$device_id/command
topic read devices/$device_id/watch
topic write devices/$device_id/presence
topic write devices/$device_id/state
topic write devices/$device_id/event
topic write devices/$device_id/ack
user relay
topic write devices/$device_id/command
topic write devices/$device_id/watch
topic read devices/$device_id/presence
topic read devices/$device_id/state
topic read devices/$device_id/event
topic read devices/$device_id/ack
EOF
# Broker drops privileges to its container UID (1883); public CA is also read by Node.
chmod 755 secrets
chmod 644 secrets/ca.crt secrets/broker.crt secrets/acl secrets/passwords
chmod 600 secrets/broker.key secrets/ca.key
chown 1883:1883 secrets/broker.key
cat > .env <<EOF
WEB_DOMAIN=$web_domain
MQTT_DOMAIN=$mqtt_domain
WEB_USER=field
WEB_PASSWORD_HASH='$web_hash'
INTERNAL_RELAY_KEY=$internal_key
RELAY_MQTT_PASSWORD=$relay_password
DEVICE_ID=$device_id
WEB_SOURCE=../web
EOF
cat > secrets/device-settings.txt <<EOF
Broker: $mqtt_domain:8883
Device ID and MQTT user: $device_id
MQTT password: $device_password
Provision secrets/ca.crt into the SARA-R4 certificate store as relay-ca.
Browser: https://$web_domain
Login: field
Browser password: $web_password
EOF
python3 "$base/export_settings.py" "$PWD"
cd "$base"
mv -- "$work/secrets" ./secrets
mv -- "$work/.env" ./.env
rmdir -- "$work"
echo 'Created .env and secrets/device-settings.txt. Save these privately.'
echo 'Move secrets/ca.key to encrypted offline storage after signing the broker certificate.'
echo 'Select WEB_SOURCE in .env, then run: docker compose up -d --build'
