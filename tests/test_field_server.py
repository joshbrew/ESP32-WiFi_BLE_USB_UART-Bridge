"""Installer contracts with fake package/container commands; no deployment."""
import importlib.util,json,os,shutil,subprocess,tempfile,unittest
from pathlib import Path

REPO=Path(__file__).resolve().parents[1]
RELAY=REPO/'ESP32S3_Modular_Command_Transport_Controller'/'relay'
spec=importlib.util.spec_from_file_location('pair_export',RELAY/'export_settings.py')
exporter=importlib.util.module_from_spec(spec);spec.loader.exec_module(exporter)
BASH=os.environ.get('FIELD_TEST_BASH') or (str(Path(os.environ.get('ProgramFiles','C:/Program Files'))/'Git/bin/bash.exe') if os.name=='nt' else shutil.which('bash'))

class ServerSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='field-server-tests-')
        self.root=Path(self.temp.name).resolve()
    def tearDown(self):
        if not self.root.is_relative_to(Path(tempfile.gettempdir()).resolve()) or not self.root.name.startswith('field-server-tests-'):
            raise RuntimeError('Unexpected temporary cleanup path')
        self.temp.cleanup()
    def seed(self,root):
        root.mkdir(parents=True,exist_ok=True);secret=root/'secrets';secret.mkdir()
        (root/'.env').write_text("WEB_DOMAIN=control.example.com\nMQTT_DOMAIN=mqtt.example.com\nDEVICE_ID=s3-controller\nWEB_USER=field\nINTERNAL_RELAY_KEY=NEVER_EXPORT_PROXY\nRELAY_MQTT_PASSWORD=NEVER_EXPORT_RELAY\n")
        (secret/'device-settings.txt').write_text('Broker: mqtt.example.com:8883\nDevice ID and MQTT user: s3-controller\nMQTT password: test-device-only\nLogin: field\nBrowser password: test-browser-only\n')
        (secret/'ca.crt').write_text('-----BEGIN CERTIFICATE-----\nAAECAwQ=\n-----END CERTIFICATE-----\n')
        (secret/'ca.key').write_text('NEVER_EXPORT_SIGNING_KEY')
    def bash(self,script,*args,ok=True):
        result=subprocess.run([BASH,'-c',script,'test',str(RELAY/'install.sh').replace('\\','/'),str(self.root).replace('\\','/'),*args],text=True,capture_output=True,timeout=30)
        if ok:self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return result
    def test_pairing_export_excludes_host_keys_and_preserves_credentials(self):
        self.seed(self.root/'relay');root=self.root/'relay'
        data=exporter.export_settings(root);file=root/'secrets/device-settings.json'
        self.assertEqual(data['mqttPassword'],'test-device-only');self.assertEqual(data['webPassword'],'test-browser-only')
        self.assertNotIn('NEVER_EXPORT',file.read_text());before=file.read_bytes()
        exporter.export_settings(root);self.assertEqual(file.read_bytes(),before)
        (root/'.env').write_text((root/'.env').read_text().replace('s3-controller','other-device'))
        with self.assertRaises(ValueError):exporter.export_settings(root)
        self.assertEqual(file.read_bytes(),before)
    def test_existing_server_reruns_without_rotating_ca_or_passwords(self):
        root=self.root/'deployment';self.seed(root)
        shutil.copyfile(RELAY/'export_settings.py',root/'export_settings.py')
        before=(root/'secrets/device-settings.txt').read_bytes();ca=(root/'secrets/ca.crt').read_bytes()
        script='''source "$1"
python3(){ local args=();for arg in "$@";do if [[ $arg == /[a-z]/* ]] && command -v cygpath >/dev/null;then args+=("$(cygpath -w "$arg")");else args+=("$arg");fi;done;python "${args[@]}"; }
# Permission bits are outside this mocked installer test (also works in Git Bash).
install(){ mkdir -p "${@: -1}"; };chmod(){ :; }
cd "$2/deployment"
configure_relay control.example.com mqtt.example.com s3-controller s3-dispenser
configure_relay control.example.com mqtt.example.com s3-controller s3-dispenser
'''
        self.bash(script)
        self.assertEqual((root/'secrets/device-settings.txt').read_bytes(),before)
        self.assertEqual((root/'secrets/ca.crt').read_bytes(),ca)
        self.assertEqual((root/'backups/ca.key').read_text(),'NEVER_EXPORT_SIGNING_KEY')
        self.assertFalse((root/'secrets/ca.key').exists())
        self.assertEqual(json.loads((root/'secrets/device-settings.json').read_text())['variant'],'s3-dispenser')
        self.assertIn('WEB_SOURCE=../../ESP32S3_Dispenser_Controller/web',(root/'.env').read_text())
        result=self.bash('source "$1"; cd "$2/deployment"; configure_relay wrong.example.com mqtt.example.com s3-controller s3-dispenser',ok=False)
        self.assertNotEqual(result.returncode,0);self.assertEqual((root/'secrets/device-settings.txt').read_bytes(),before)
    def test_docker_install_and_reuse_never_remove_existing_packages(self):
        script='''source "$1"
VERSION_CODENAME=noble
installed=0
command(){ if [[ $1 == -v && $2 == docker && $installed == 0 ]];then return 1;fi;builtin command "$@"; }
apt-get(){ printf '%s\n' "$*" >> "$2_UNUSED"; }
'''
        # Functions use a separate log variable, avoiding package-manager/system changes.
        script=script[:script.index('apt-get(){')]+'''
log="$2/operations"
apt-get(){ printf 'apt %s\n' "$*" >> "$log";if [[ $* == *docker-ce* ]];then installed=1;fi; }
curl(){ while (($#));do if [[ $1 == -o ]];then printf fake-key > "$2";return;fi;shift;done; }
dpkg(){ echo amd64; }
systemctl(){ printf 'systemctl %s\n' "$*" >> "$log"; }
docker(){ printf 'docker %s\n' "$*" >> "$log";return 0; }
install_prerequisites
install_docker "$2/apt"
before=$(grep -c '^apt ' "$log")
install_docker "$2/apt"
after=$(grep -c '^apt ' "$log")
[[ $before == "$after" ]]
'''
        self.bash(script)
        operations=(self.root/'operations').read_text()
        self.assertIn('docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin',operations)
        self.assertIn('ca-certificates curl openssl python3 tar',operations)
        self.assertIn('--no-remove',operations);self.assertNotIn('uninstall',operations)
    def test_dry_run_validation_and_shell_syntax(self):
        self.bash('bash -n "$1"; bash -n "$(dirname "$1")/setup.sh"; bash "$1" --web-domain control.example.com --mqtt-domain mqtt.example.com --dry-run')
        result=self.bash('bash "$1" --web-domain "evil.example.com;touch" --mqtt-domain mqtt.example.com --dry-run',ok=False)
        self.assertNotEqual(result.returncode,0)
    def test_failed_initial_setup_does_not_leave_live_credentials(self):
        destination=self.root/'deployment';destination.mkdir()
        shutil.copyfile(RELAY/'export_settings.py',destination/'export_settings.py')
        # Fail the first container command after staged certificate generation.
        result=self.bash('''
setup="$(dirname "$1")/setup.sh"
openssl(){ :; };docker(){ return 42; }
cd "$2/deployment"
source "$setup" control.example.com mqtt.example.com s3-controller
''',ok=False)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse((destination/'.env').exists());self.assertFalse((destination/'secrets').exists())
        self.assertEqual(list(destination.glob('.relay-setup-*')),[])

if __name__=='__main__':unittest.main()
