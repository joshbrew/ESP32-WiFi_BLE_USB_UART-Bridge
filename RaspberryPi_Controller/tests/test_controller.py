import asyncio
import copy
import json
import tempfile
import time
import unittest
from pathlib import Path

from bridge.config import DEFAULTS, validate, load_config
from bridge.core import Controller, AdmissionError
from bridge.dispenser import Dispenser
from bridge.hardware import SimulatedGPIO, PiGPIO
from bridge.streams import LineInput


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class Output:
    ready = True

    def __init__(self):
        self.messages = []

    def send(self, text):
        self.messages.append(text)


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.config = copy.deepcopy(DEFAULTS)
        self.config.update(max_pulse_ms=60000, arm_timeout_ms=120000)
        self.config['data_dir'] = self.directory.name
        self.clock = Clock()
        self.gpio = SimulatedGPIO(self.config)
        self.controller = Controller(self.config, self.gpio, self.clock, watchdog=False)

    def tearDown(self):
        self.controller.dispenser.close()
        self.directory.cleanup()

    def command(self, line):
        self.controller.execute(line)

    def test_boot_and_full_pulse(self):
        with self.assertRaises(ValueError):
            self.command('Dispense:250')
        self.command('Arm')
        self.assertFalse(self.gpio.active)
        self.command('Dispense:250')
        self.clock.advance(.249)
        self.controller.dispenser.tick()
        self.assertTrue(self.gpio.active)
        self.clock.advance(.002)
        self.controller.dispenser.tick()
        self.assertFalse(self.gpio.active)
        self.assertTrue(self.controller.dispenser.state()['armed'])

    def test_overlap_limits_and_arm_expiry(self):
        self.command('Arm')
        self.command('Dispense:250')
        for line in ('Dispense:1', 'Arm', 'Dispense:60001', 'Dispense:-1', 'Dispense:0'):
            with self.assertRaises(ValueError):
                self.command(line)
        self.command('DispenseStop')
        self.clock.advance(119.9)
        with self.assertRaises(ValueError):
            self.command('Dispense:250')
        self.clock.advance(.2)
        self.controller.dispenser.tick()
        self.assertFalse(self.controller.dispenser.state()['armed'])

    def test_interlock_fault_requires_disarm(self):
        self.command('Arm')
        self.command('Dispense:250')
        self.gpio.interlock_open = True
        self.controller.dispenser.tick()
        self.assertFalse(self.gpio.active)
        self.assertTrue(self.controller.dispenser.faulted)
        self.gpio.interlock_open = False
        with self.assertRaises(ValueError):
            self.command('Arm')
        self.controller.submit('Disarm')
        self.command('Arm')

    def test_stop_barrier_and_dedup(self):
        self.controller.submit('Arm\nDispense:100', request_id='pulse1')
        self.assertEqual(self.controller.queue.qsize(), 2)
        self.assertTrue(self.controller.submit('Arm\nDispense:100', request_id='pulse1')['duplicate'])
        self.assertEqual(self.controller.queue.qsize(), 2)
        with self.assertRaises(AdmissionError):
            self.controller.submit('Arm', request_id='pulse1')
        self.command('Arm')
        self.command('Dispense:100')
        self.controller.submit('StopAll\nArm\nDispense:100')
        self.assertEqual(self.controller.queue.qsize(), 0)
        self.assertFalse(self.gpio.active)
        self.assertFalse(self.controller.dispenser.state()['armed'])

    def test_batch_admission_is_atomic(self):
        for _ in range(7):
            self.controller.submit('Ping')
        with self.assertRaises(AdmissionError) as raised:
            self.controller.submit('Arm\nDispense:100')
        self.assertEqual(raised.exception.status, 503)
        self.assertEqual(self.controller.queue.qsize(), 7)
        for body in ('', 'x' * 257, 'Ping\n' * 9, 'Ping\0'):
            with self.assertRaises(AdmissionError):
                self.controller.submit(body)

    def test_retry_history_retains_unexpired_ids(self):
        for index in range(64):
            self.controller.submit('Ping', request_id=str(index))
            self.controller.queue.get_nowait()
            self.controller.queue.task_done()
        with self.assertRaises(AdmissionError):
            self.controller.submit('Arm', request_id='next')
        self.assertTrue(self.controller.submit('Ping', request_id='0')['duplicate'])
        self.controller.submit('StopAll', request_id='stop')
        self.clock.advance(31)
        self.assertTrue(self.controller.submit('Ping', request_id='next')['accepted'])

    def test_profiles_persist_only_configuration(self):
        self.command('DispenserDefaultPulse:150')
        self.command('DispenserMaxPulse:750')
        self.command('PayloadProfileSave:Fine')
        self.command('Arm')
        with self.assertRaises(ValueError):
            self.command('PayloadProfileUse:fine')
        self.command('Dispense:100')
        self.controller.dispenser.close()
        reboot_gpio = SimulatedGPIO(self.config)
        reboot = Controller(self.config, reboot_gpio, self.clock, watchdog=False)
        try:
            self.assertEqual(reboot.dispenser.settings['maxPulseMs'], 750)
            self.assertEqual(reboot.selected, 'fine')
            self.assertFalse(reboot_gpio.active)
            self.assertFalse(reboot.dispenser.state()['armed'])
        finally:
            reboot.dispenser.close()

    def test_configuration_ceiling_and_reserved_pin(self):
        for line in ('DispenserPin:14', 'DispenserActiveHigh:maybe', 'DispenserMaxPulse:60001', 'DispenserArmTimeout:120001'):
            with self.assertRaises(ValueError):
                self.command(line)
        self.command('DispenserPin:22')
        self.assertEqual(self.gpio.pin, 22)

    def build_routine(self, name='dots', steps=None):
        self.command('RoutineCreate:' + name)
        for step in steps or ['DISPENSE:100', 'WAIT_IDLE', 'WAIT:10']:
            self.command(f'RoutineAdd:{name}:{step}')
        self.command(f'RoutineSave:{name}')

    def test_routine_completes_last_pulse_and_disarms(self):
        self.build_routine(steps=['DISPENSE:100'])
        self.command('RoutineRepeat:dots:2')
        self.command('Arm')
        self.command('RoutineRun:dots')
        self.controller.routines.tick()
        self.clock.advance(.05)
        self.controller.routines.tick()
        self.assertTrue(self.gpio.active)
        self.clock.advance(.06)
        self.controller.routines.tick()
        self.assertEqual(self.controller.dispenser.count, 2)
        self.assertTrue(self.gpio.active)
        self.clock.advance(.11)
        self.controller.routines.tick()
        self.assertFalse(self.gpio.active)
        self.assertFalse(self.controller.routines.running)
        self.assertFalse(self.controller.dispenser.state()['armed'])

    def test_routine_blocks_manual_start_and_administration(self):
        self.build_routine()
        for spec in ('COMMAND:Arm', 'COMMAND:Reboot', 'COMMAND:Send:hello', 'COMMAND:RoutineRun:dots', 'COMMAND:DispenserPin:22'):
            with self.assertRaises(ValueError):
                self.command('RoutineAdd:dots:' + spec)
        self.command('Arm')
        self.command('RoutineRun:dots')
        for line in ('Arm', 'Dispense:100', 'PayloadProfileSave:fine', 'RoutineCreate:other'):
            with self.assertRaises(ValueError):
                self.command(line)
        self.controller.routines.tick()
        self.controller.submit('StopAll')
        self.assertFalse(self.gpio.active)
        self.assertFalse(self.controller.routines.running)

    def test_routine_failure_safes_hardware(self):
        self.build_routine(steps=['DISPENSE:100', 'DISPENSE:100'])
        self.command('Arm')
        self.command('RoutineRun:dots')
        self.controller.routines.tick()
        self.controller.routines.tick()
        self.assertFalse(self.gpio.active)
        self.assertIn('failed', self.controller.routines.last_result)

    def test_corrupt_state_fails_closed(self):
        self.command('DispenserSave')
        path = self.controller.store.path
        record = json.loads(path.read_text())
        record['data']['settings']['pin'] = 14
        path.write_text(json.dumps(record))
        gpio = SimulatedGPIO(self.config)
        with self.assertRaises(ValueError):
            Controller(self.config, gpio, self.clock, watchdog=False)
        self.assertFalse(gpio.active)

    def test_saved_polarity_resolved_before_gpio_creation(self):
        self.command('DispenserActiveHigh:OFF')
        self.command('DispenserPin:22')
        self.command('DispenserSave')
        created = []
        def factory(config):
            created.append((config['dispenser_pin'], config['active_high']))
            return SimulatedGPIO(config)
        reboot = Controller(self.config, clock=self.clock, watchdog=False, gpio_factory=factory)
        try:
            self.assertEqual(created, [(22, False)])
            self.assertFalse(reboot.dispenser.state()['armed'])
        finally:
            reboot.dispenser.close()

    def test_send_has_no_automatic_cross_transport_echo(self):
        usb, ble, uart = Output(), Output(), Output()
        self.controller.outputs = {'USB': usb, 'BLE': ble, 'UART': uart}
        self.command('Ping')
        self.assertTrue(usb.messages)
        self.assertFalse(ble.messages)
        self.controller.execute('SendUART:payload', 'WiFi')
        self.assertIn('payload\n', uart.messages)
        self.assertNotIn('payload\n', usb.messages)
        self.controller.execute('Send:all', 'BLE')
        self.assertIn('all\n', usb.messages)
        self.assertNotIn('all\n', ble.messages)

    def test_stream_chunks_utf8_and_overflow(self):
        output = Output()
        stream = LineInput(self.controller, 'BLE', output.send)
        stream.feed(b'Pi')
        stream.feed(b'ng\n')
        self.assertEqual(self.controller.queue.qsize(), 1)
        stream.feed(b'x' * 400 + b'\nPing\n')
        self.assertEqual(self.controller.queue.qsize(), 2)
        stream.feed(b'\xff\n')
        stream.feed(b'@STATE\n')
        self.assertTrue(any(line.startswith('@STATE ') for line in output.messages))

    def test_event_cursor_resets_after_restart(self):
        page = self.controller.event_page(10000, 8)
        self.assertTrue(page['gap'])
        self.assertEqual(page['cursor'], self.controller.latest)
        self.assertTrue(page['events'])

    def test_watchdog_stops_without_event_loop(self):
        gpio = SimulatedGPIO(self.config)
        dispenser = Dispenser(self.config, gpio)
        try:
            dispenser.arm()
            dispenser.dispense(30)
            time.sleep(.12)
            self.assertFalse(gpio.active)
        finally:
            dispenser.close()


class GPIOTests(unittest.TestCase):
    def test_default_and_example_configuration_load(self):
        validate(copy.deepcopy(DEFAULTS))
        config = load_config(Path(__file__).resolve().parent.parent / 'config.example.json')
        self.assertTrue(config['simulate'])

    def test_actual_driver_with_gpiozero_mock_pins(self):
        from gpiozero import Device
        from gpiozero.pins.mock import MockFactory
        prior = Device.pin_factory
        Device.pin_factory = MockFactory()
        try:
            config = copy.deepcopy(DEFAULTS)
            config['interlock_pin'] = 17
            gpio = PiGPIO(config)
            self.assertTrue(gpio.is_interlock_open())
            Device.pin_factory.pin(17).drive_low()
            self.assertFalse(gpio.is_interlock_open())
            gpio.write(True)
            self.assertTrue(Device.pin_factory.pin(26).state)
            gpio.configure(22, False)
            self.assertTrue(Device.pin_factory.pin(22).state)
            gpio.write(True)
            self.assertFalse(Device.pin_factory.pin(22).state)
            gpio.close()
        finally:
            Device.pin_factory = prior

    def test_config_rejects_pin_collisions_and_boolean_numbers(self):
        for override in ({'dispenser_pin': True}, {'max_pulse_ms': True}, {'interlock_pin': 26}, {'uart_device': '/dev/x', 'usb_device': '/dev/x'}):
            with self.assertRaises(ValueError):
                validate(dict(DEFAULTS, **override))


class HTTPTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestServer, TestClient
        from bridge.server import create_app
        self.directory = tempfile.TemporaryDirectory()
        config = dict(DEFAULTS, data_dir=self.directory.name)
        self.controller = Controller(config, SimulatedGPIO(config))
        self.client = TestClient(TestServer(create_app(self.controller)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        self.directory.cleanup()

    async def test_portal_and_command_retry(self):
        response = await self.client.get('/')
        self.assertIn('Raspberry Pi', await response.text())
        for path in ('/app.js', '/app.css', '/api/ping', '/api/state', '/api/events'):
            self.assertEqual((await self.client.get(path)).status, 200)
        headers = {'X-Request-ID': 'test1', 'Content-Type': 'text/plain'}
        response = await self.client.post('/api/command', data='Arm\nDispense:100', headers=headers)
        self.assertEqual(response.status, 202)
        response = await self.client.post('/api/command', data='Arm\nDispense:100', headers=headers)
        self.assertTrue((await response.json())['duplicate'])
        await self.controller.queue.join()
        self.assertEqual(self.controller.dispenser.count, 1)
        response = await self.client.post('/api/command', data='StopAll')
        self.assertEqual(response.status, 202)
        self.assertFalse(self.controller.dispenser.state()['armed'])

    async def test_invalid_requests_and_cross_origin(self):
        self.assertEqual((await self.client.get('/api/events?since=bad')).status, 400)
        response = await self.client.post('/api/command', data='Arm', headers={'Origin': 'https://unrelated.example'})
        self.assertEqual(response.status, 403)
        self.assertFalse(self.controller.dispenser.state()['armed'])
        self.assertEqual((await self.client.post('/api/command', data='x' * 3000)).status, 413)
        self.assertEqual((await self.client.post('/api/command', json={'command': 'Arm'})).status, 415)
        self.assertEqual((await self.client.post('/api/ota', data=b'\xe9' * 1024)).status, 403)

    async def test_storage_failure_does_not_claim_saved_profile(self):
        from unittest.mock import patch
        with patch.object(self.controller.store, 'save', side_effect=OSError('disk full')):
            await self.client.post('/api/command', data='PayloadProfileSave:new')
            await self.controller.queue.join()
        self.assertNotIn('new', self.controller.profiles)
        self.assertEqual(self.controller.selected, '')
        self.assertFalse(self.controller.dispenser.state()['armed'])


class BLESchemaTests(unittest.TestCase):
    def test_bluez_object_tree_and_uart_payload(self):
        from bridge.ble import BLETransport, Application, UARTService, SERVICE_PATH, RX_PATH, TX_PATH
        config = dict(DEFAULTS)
        with tempfile.TemporaryDirectory() as directory:
            config['data_dir'] = directory
            controller = Controller(config, SimulatedGPIO(config), watchdog=False)
            try:
                transport = BLETransport(controller)
                # dbus-next method decorators invoke the method but discard its
                # result when called directly; inspect exported method metadata.
                from dbus_next.service import ServiceInterface
                method = ServiceInterface._get_methods(Application(UARTService(), transport.rx, transport.tx))[0]
                app = Application(UARTService(), transport.rx, transport.tx)
                objects = method.fn(app)
                self.assertEqual(set(objects), {SERVICE_PATH, RX_PATH, TX_PATH})
                transport.tx.StartNotify()
                transport.rx.WriteValue(b'Ping\n', {})
                self.assertEqual(controller.queue.qsize(), 1)
                self.assertTrue(transport.ready)
                transport.tx.StopNotify()
                self.assertFalse(transport.ready)
            finally:
                controller.dispenser.close()


if __name__ == '__main__':
    unittest.main()
