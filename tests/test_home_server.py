import importlib.util,json,os,subprocess,tempfile,unittest,sys
from pathlib import Path
from unittest.mock import patch

RELAY=Path(__file__).resolve().parents[1]/'ESP32S3_Modular_Command_Transport_Controller/relay'
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('home_prepare',RELAY/'home_prepare.py')
home=importlib.util.module_from_spec(spec);spec.loader.exec_module(home)
OPENSSL=Path(r'C:\Program Files\Git\usr\bin\openssl.exe')

class HomeServerTests(unittest.TestCase):
    def test_real_ca_and_broker_certificate_with_isolated_secrets(self):
        if not OPENSSL.exists():self.skipTest('Install OpenSSL to run real certificate check')
        with tempfile.TemporaryDirectory(prefix='home-cert-tests-') as temporary:
            root=Path(temporary)
            real_run=subprocess.run
            def run(argv,**kwargs):return real_run([str(OPENSSL),*argv[1:]],**kwargs)
            # Only substitute the binary location, not certificate behavior.
            with patch.object(home.subprocess,'run',side_effect=run):
                home.prepare(root,'control.example.com','mqtt.example.com','s3-controller','s3-dispenser')
            verified=real_run([str(OPENSSL),'verify','-CAfile',str(root/'secrets/ca.crt'),'-verify_hostname','mqtt.example.com',str(root/'secrets/broker.crt')],capture_output=True,text=True)
            self.assertEqual(verified.returncode,0,verified.stderr)
            wrong=real_run([str(OPENSSL),'verify','-CAfile',str(root/'secrets/ca.crt'),'-verify_hostname','wrong.example.com',str(root/'secrets/broker.crt')],capture_output=True,text=True)
            self.assertNotEqual(wrong.returncode,0)
            self.assertFalse((root/'secrets/ca.key').exists())
            self.assertTrue((root/'backups/ca.key').exists())
            values=json.loads((root/'bootstrap.json').read_text())
            self.assertEqual(len(values['devicePassword']),48)
            self.assertNotEqual(values['devicePassword'],values['relayPassword'])
            self.assertNotEqual(values['webPassword'],values['devicePassword'])
            self.assertIn('topic read devices/s3-controller/command',(root/'secrets/acl').read_text())
            before=(root/'secrets/ca.crt').read_bytes()
            with self.assertRaises(ValueError):home.prepare(root,'control.example.com','mqtt.example.com','s3-controller','s3-dispenser')
            self.assertEqual((root/'secrets/ca.crt').read_bytes(),before)
    def test_invalid_inputs_fail_before_writing(self):
        with tempfile.TemporaryDirectory(prefix='home-invalid-tests-') as temporary:
            for args in [('https://bad.example.com','mqtt.example.com','ok','s3-dispenser'),('control.example.com','bad;cmd.example.com','ok','s3-dispenser'),('control.example.com','mqtt.example.com','relay','s3-dispenser'),('control.example.com','mqtt.example.com','ok','lolin32-dispenser')]:
                with self.assertRaises(ValueError):home.prepare(temporary,*args)
            self.assertEqual(list(Path(temporary).iterdir()),[])

if __name__=='__main__':unittest.main()
