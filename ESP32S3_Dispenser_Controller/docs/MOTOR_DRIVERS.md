# DC motors, TMC2209 steppers and pin assignment

Use the **S3 dispenser** or **LOLIN32 dispenser** firmware and its matching
standalone console. The transport-only builds do not drive motors. Dispenser
controls stay first in the side panel; DRV8871 and TMC2209 panels follow them.
Both panels stay visible without hardware support and explain when the
connected firmware needs an update. An HTML file cannot enable code missing
from an older firmware binary.

## Enable support once

The features remain opt-in. When ready to compile, from the repository root:

```text
controller.cmd compile -Variant s3-dispenser -MotorDrivers
controller.cmd flash -Variant s3-dispenser
```

Use `lolin32-dispenser` for the classic board. `-MotorDrivers` enables both
drivers, installs missing tools/libraries, and keeps a separate build cache.
`controller.cmd setup -MotorDrivers` installs the prerequisites without building.
You can combine `-MotorDrivers` with `-Lte` on the S3 after LTE setup.
Flashing still asks you to confirm the detected board and COM port.

Keep `DRONE_CFG_HARDWARE_PROFILE` at `1` for these drivers. Profile `2`
selects the older ULN2003/DAC bench hardware.

In Arduino IDE, set `DRONE_CFG_ENABLE_DRV8871` and/or
`DRONE_CFG_ENABLE_TMC2209` to `1` in `QuickConfig.h`. Install **TMCStepper 0.7.3**
and **FastAccelStepper 2.0.1** when enabling the stepper. Board settings remain
the ones in the firmware README. Enabling both drivers reserves their default
pins until you assign replacements and restart.

This implementation has native tests and browser simulations; it has not yet
been compiled for the ESP32 or tested with real motors. No builds or flashes
were performed as part of this change.

## Assign pins without editing firmware

1. Connect the console and **Disarm**. Stop any saved routine or coordinate run.
2. Expand **Assign pins / add motor** in the driver's panel.
3. Choose motor 1 to edit it, or the next consecutive number to add another.
   There are four slots per type, subject to available GPIOs and pulse resources.
4. Enter IN1/IN2 for DRV8871, or STEP/DIR/EN and UART address for TMC2209.
   The TMC panel also has a shared UART TX/RX pin editor.
5. Choose **Save pins**, then **Restart to apply pins**. Rewire with motor power
   off. Saved assignments survive restarts; the controller always boots disarmed.

Saved changes block arming until restart. Existing pins remain active/reserved
until that restart, and desired new pins are reserved immediately. No pulse
generator is rebound while it is running. Pin maps are stored together in one
small checksummed record and read back after saving. Invalid/corrupt records
fall back to validated compiled defaults. Removing a driver removes only the
last motor number, preserving the IDs of all earlier motors. Removing the last
channel disables that family and frees its GPIOs after restart.

The firmware rejects duplicate GPIOs, reserved USB/flash/PSRAM/LED pins,
interlock/payload outputs, modem/auxiliary UART conflicts and duplicate TMC
addresses. Pins must be output-capable members of the board's conservative
allowlist. There may be fewer physical pin combinations than driver slots.
Changing a motor's wiring changes what its saved motor number drives: review
saved routines before arming after a wiring change.

Serial/API equivalents:

```text
DriverPinsStatus
MotorPins:1,6,7
MotorPins:2,13,14
TMCPins:1,8,9,10,0
TMCPins:2,15,16,21,1
TMCUART:11,12
MotorPins:2,OFF
DriverPinsReset
Reboot
```

These GPIO examples are for the S3 and illustrate separate commands, not a
required batch. `DriverPinsReset` restores the compiled motor pin map and also
requires restart. Current and microstep adjustments are runtime settings for
this boot; compile defaults restore on restart.

## Default wiring

| Connection | ESP32-S3 N16R8 | Classic LOLIN32 |
| --- | --- | --- |
| DRV8871 motor 1 IN1 / IN2 | GPIO6 / GPIO7 | GPIO32 / GPIO33 |
| TMC motor 1 STEP / DIR / EN | GPIO8 / GPIO9 / GPIO10 | GPIO18 / GPIO19 / GPIO21 |
| ESP32 TMC UART TX / RX | GPIO11 / GPIO12 | GPIO22 / GPIO27 |
| TMC UART controller | UART1 | UART1 |
| TMC motor 1 UART address | 0 | 0 |

The S3 LTE modem stays on UART2 / GPIO17+18. USB data, onboard RGB and N16R8
memory pins are not used by these motor defaults. The classic UART2 is free
unless configured otherwise. Optional auxiliary UART1 cannot share a hardware
UART with the TMC2209 bus.

For DRV8871, connect motor power to the breakout's motor supply and the two
motor leads to its outputs. Connect common ground and the assigned IN1/IN2.
Drive level is PWM 0..255; 0 coasts. The firmware uses drive/brake PWM and
coasts at the end of a timed pulse. See the
[Adafruit pinout](https://learn.adafruit.com/adafruit-drv8871-brushed-dc-motor-driver-breakout/pinouts).

### BIGTREETECH TMC2209 V1.3 + 0.5 A, 1.8 degree NEMA 14

Connect VIO to ESP32 **3.3 V**, both grounds together, and VM to a separate,
current-capable motor supply (12 V is a suitable starting setup for this
current-regulated driver; stay within the module's documented supply range).
Place suitable bulk capacitance close to VM/GND. Never power the motor from
an ESP32 GPIO/3.3 V pin, and disconnect motor power before changing wiring.

Connect STEP, DIR and active-low EN to their assigned ESP32 pins. Identify the
two winding pairs by continuity: one pair goes to A1/A2, the other to B1/B2.
Do not infer winding pairs from the wire colors alone.

**V1.3 UART detail:** its header RX pin connects to PDN_UART. The adjacent
header TX pin is not connected by default; R10 bridges the two pins if modified.
Leave that TX pin unused for the following single-wire arrangement:

```text
ESP32 UART TX ----- 1 kOhm -----+----- BTT header RX / PDN_UART
ESP32 UART RX -----------------+
ESP32 GND --------------------------- BTT GND
```

Do not assume the two labels form a conventional crossed RX/TX UART. Inspect
the actual V1.3 module before connecting it. This arrangement follows the
[BTT V1.3 schematic](https://github.com/bigtreetech/BIGTREETECH-Stepper-Motor-Driver/blob/master/TMC2209/V1.3/Schematic/TMC2209%20V1.3-SCH.pdf)
and [V1.3 manual](https://github.com/bigtreetech/BIGTREETECH-Stepper-Motor-Driver/blob/master/TMC2209/V1.3/manual/BIGTREETECH%20TMC2209%20V1.3%20User%20Manual.pdf).

Multiple TMC drivers share that UART node and common ground; each gets its
own STEP/DIR/EN pins and a unique MS1/MS2 address:

| UART address | MS1 | MS2 |
| --- | --- | --- |
| 0 | GND | GND |
| 1 | 3.3 V | GND |
| 2 | GND | 3.3 V |
| 3 | 3.3 V | 3.3 V |

Defaults are **200 full steps/revolution**, **16 microsteps**, **300 mA RMS**,
with a **350 mA RMS ceiling**. The ceiling is a conservative choice for the
reported 0.5 A/phase motor (350 mA RMS is about 495 mA sine peak), rather than
the driver's 2.8 A marketing peak. Requested and quantized programmed RMS settings
are shown separately; the values are calculated from register settings. The supplied BTT schematic specifies 0.11 ohm sense
resistors. Check the actual module if it differs. UART mode disables analog
VREF scaling and sets the digital current limit; copying a high factory VREF
setting from another motor is unnecessary.
See the [TMC2209 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf)
for current scaling, single-wire wiring and address selection.

After powering the driver, disarm and choose **Check UART** / `TMCInit`.
Initialization checks chip identity, write counter, digital current mode,
microstep configuration and the acknowledgement of the write-only current register. Motion is rejected until these
checks pass. Each new motion also checks UART identity/configuration, catching
a power-cycle that reset the digital current-control mode. An explicit
disarm/reinitialization is required after a failed check.

## Sequence normally, prime concurrently

Use the regular routine builder for saved GPIO, DRV8871 or TMC2209 pulse/gap
sequences. Motors run in sequence in a routine. A TMC step is:

```text
RoutineCreate:stepper
RoutineAdd:stepper:TMC:1000,1000,30,1,1
RoutineRepeat:stepper:6
RoutineSave:stepper
Arm
RoutineRun:stepper
```

The TMC fields are onMs, offMs, RPM, direction (+1/-1), optional motor number.
Off periods must be positive. FOREVER uses the same explicit positive gaps.
Routine/coordinate ownership blocks manual starts/configuration until stopped.

For priming, arm and start/pulse different selected motors, or use **Prime all**
to pulse all configured channels of that type using the displayed level or
RPM/direction/duration. Manual DC and TMC channels may overlap. Each channel
has its own timer. Starting an already-running channel is rejected; changing
the selected DC drive level adjusts only that motor and keeps its deadline.
GPIO dispensing does not overlap motor priming.

`MotorStop:2` brakes only DC motor 2 while armed; `MotorCoast:2` releases it.
The unnumbered commands apply to all DC motors. `TMCStop:2`/`TMCDisable:2`
stop only stepper 2 and release its coils; unnumbered commands stop all steppers.
Any motor stop during a saved routine cancels that automation and stops its
outputs, so the next pulse cannot restart it. **Stop all**, **Disarm**,
`DispenseStop`, interlock closure and arm expiry stop every motor/output.

DC runs default to a 30 s cap. TMC runs are limited to 30 s and 120 RPM in this
motor profile; shorter payload/arm limits still apply. A stopped TMC motor is
disabled, with no holding torque. STEP pulses use hardware-timed acceleration;
the queued distance and requested runtime are bounded. Degrees are relative
commanded motion. There is no encoder, homing or guaranteed position after a
stall/power cycle; **Check UART** does not find a mechanical zero.

Useful stepper commands:

```text
TMCStatus
TMCInit
TMCMove:360,30,1
TMCMove:-90,20,2
TMCRun:30,1,1
TMCPulse:1000,30,-1,2
TMCStop:2
TMCDisable
TMCCurrent:300,1
TMCMicrosteps:16,1
```

Current/microstep changes require disarming; microstep changes reset the
software position counter. No feature automatically arms or starts at boot.

Individual motor stops stay in the normal ordered command queue, preserving
each requested target during concurrent priming. If a queue is busy or immediate
global stopping is needed, use **Stop all** or **Disarm**: those retain the
priority queue-clearing path. Normal-queue rejection is reported to the caller.
