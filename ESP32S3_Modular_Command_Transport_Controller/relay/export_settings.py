#!/usr/bin/env python3
"""Export only device pairing data; never proxy credentials or signing keys."""
import hashlib,json,os,re,ssl,sys,tempfile
from pathlib import Path

def export_settings(root):
    root=Path(root);secrets=root/'secrets'
    values={}
    for line in (root/'.env').read_text().splitlines():
        if '=' in line and not line.startswith('#'):
            key,value=line.split('=',1);values[key]=value.strip().strip("'\"")
    text=(secrets/'device-settings.txt').read_text()
    def field(label):
        match=re.search(r'^'+re.escape(label)+r': (.+)$',text,re.M)
        if not match:raise ValueError('Existing device settings are incomplete; credentials were preserved')
        return match[1].strip()
    host,port=field('Broker').rsplit(':',1)
    device=field('Device ID and MQTT user')
    if host!=values.get('MQTT_DOMAIN') or device!=values.get('DEVICE_ID') or port!='8883':
        raise ValueError('Existing .env and device settings disagree; credentials were preserved')
    pem=(secrets/'ca.crt').read_text(encoding='ascii')
    der=ssl.PEM_cert_to_DER_cert(pem)
    data={'version':1,'deviceId':device,'mqttHost':host,'mqttPort':8883,'mqttUser':device,
          'mqttPassword':field('MQTT password'),'caName':'relay-ca','caPem':pem,
          'caSha256':hashlib.sha256(der).hexdigest(),
          'webUrl':'https://'+values['WEB_DOMAIN'],'webUser':field('Login'),
          'webPassword':field('Browser password'),'variant':values.get('FIRMWARE_VARIANT','s3-transport')}
    output=json.dumps(data,indent=2)+'\n'
    target=secrets/'device-settings.json'
    if target.exists() and target.read_text()==output:
        target.chmod(0o600);return data
    fd,name=tempfile.mkstemp(prefix='.pairing-',dir=secrets)
    try:
        with os.fdopen(fd,'w') as stream:stream.write(output)
        os.chmod(name,0o600);os.replace(name,target)
    finally:
        if os.path.exists(name):os.unlink(name)
    return data

if __name__=='__main__':
    try:export_settings(Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent)
    except (ValueError,OSError,KeyError):
        sys.exit('Pairing export failed. Check .env, device-settings.txt and ca.crt; existing credentials were preserved.')
