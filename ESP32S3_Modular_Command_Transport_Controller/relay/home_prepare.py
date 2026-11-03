#!/usr/bin/env python3
"""Prepare a new Windows Docker deployment in an empty, private staging folder.

Runs inside python:3.12-alpine with OpenSSL installed; no Docker socket mount.
Passwords stay in the mounted folder and are never printed.
"""
import json, os, re, secrets, subprocess, sys
from pathlib import Path

def prepare(root, web, mqtt, device, variant):
    root = Path(root)
    for name in (web, mqtt):
        if len(name) > 253 or '.' not in name or any(not re.fullmatch(r'[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?', label) for label in name.split('.')):
            raise ValueError('Use plain DNS hostnames')
    if not re.fullmatch(r'[a-zA-Z0-9_-]{1,40}', device) or device == 'relay' or variant not in ('s3-dispenser', 's3-transport'):
        raise ValueError('Invalid device or variant')
    if (root/'secrets').exists() or (root/'.env').exists():
        raise ValueError('Existing credentials are never overwritten')
    os.umask(0o077)
    certs = root/'secrets';certs.mkdir()
    backup = root/'backups';backup.mkdir()
    def openssl(*args):
        subprocess.run(['openssl', *args], cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    openssl('genrsa','-out','backups/ca.key','2048')
    openssl('req','-x509','-new','-sha256','-days','3650','-key','backups/ca.key','-out','secrets/ca.crt',
            '-subj','/CN=Field relay CA','-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign')
    openssl('genrsa','-out','secrets/broker.key','2048')
    openssl('req','-new','-sha256','-key','secrets/broker.key','-out','secrets/broker.csr','-subj','/CN='+mqtt)
    (certs/'extensions.cnf').write_text('subjectAltName=DNS:'+mqtt+',DNS:broker\nbasicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n')
    openssl('x509','-req','-sha256','-days','365','-in','secrets/broker.csr','-CA','secrets/ca.crt',
            '-CAkey','backups/ca.key','-CAcreateserial','-extfile','secrets/extensions.cnf','-out','secrets/broker.crt')
    data = dict(devicePassword=secrets.token_hex(24), relayPassword=secrets.token_hex(24),
                internalKey=secrets.token_hex(32), webPassword=secrets.token_hex(12))
    (root/'bootstrap.json').write_text(json.dumps(data))
    acl = f'user {device}\n'
    for kind in ('command', 'watch'):acl += f'topic read devices/{device}/{kind}\n'
    for kind in ('presence','state','event','ack'):acl += f'topic write devices/{device}/{kind}\n'
    acl += 'user relay\n'
    for kind in ('command', 'watch'):acl += f'topic write devices/{device}/{kind}\n'
    for kind in ('presence','state','event','ack'):acl += f'topic read devices/{device}/{kind}\n'
    (certs/'acl').write_text(acl)

if __name__ == '__main__':
    try:prepare(*sys.argv[1:])
    except (OSError, ValueError, subprocess.CalledProcessError):sys.exit('Home credential preparation failed; no existing deployment was changed.')
