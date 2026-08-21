# SnipeIt

A remotely operated reconnaissance and fire-solution platform. A Raspberry Pi 5
runs a device layer in C that drives a servo arm and five sensors, streams video
to a Jetson Orin Nano for object detection, and serves an Android companion app
over WebSocket.

## Repository map

| Path | Contents |
|---|---|
| [`src/`](https://github.com/emilglater/SnipeIt/tree/1667748340fe89da5c506ace6532c7682206b78d/src) | Embedded layer in C: HAL, OSAL, Active Object framework, DDL sensor drivers, scheduler, broadcaster |
| [`pi-streaming/`](https://github.com/emilglater/SnipeIt/tree/1667748340fe89da5c506ace6532c7682206b78d/pi-streaming) | C server: WebSocket, video pipeline, Orin protocol, acoustic DSP |
| [`SnipeItApp/`](https://github.com/emilglater/SnipeIt/tree/1667748340fe89da5c506ace6532c7682206b78d/SnipeItApp) | Android companion app (Kotlin) |

---

## Architecture - the Active Object framework

The whole hardware layer is built around one main pattern: every peripheral is an
Active Object that runs on its own thread, with a blocking event queue, and a finite
state machine. Each module uses the event bus for communication with the others.

- **[The Active Object event loop](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/util/active_object/active_object.c#L6-L29)** -
  pop an event from the queue, break on `eFSM_EVENT_END`, otherwise dispatch
  it into the FSM.
- **[The blocking queue](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/util/queue/queue.c#L39-L88)** -
  mutex plus condition variable, so an idle Active Object thread sleeps
  instead of spinning.
- **[The FSM](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/util/fsm/fsm.h#L22-L33)** -
  three types (`StateFP`, `Event`, `FSM`) save the FSM state and enable transition.
- **[State transition](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/util/fsm/fsm.c#L37-L53)** -
  exit event, swap the state function pointer, entry event, so any state can
  pair setup in ENTRY with the matching teardown in EXIT and be certain both run.
- **[Event bus publish](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/util/event_bus/event_bus.c#L88-L113)** -
  matches on (Active Object id, event type) and calls each subscriber's own
  post function, which is why no module ever includes another module's header.

## Module registration and scheduling

![Layer diagram: APP, DDL and HAL, with each sensor grouped under the bus it uses](images/layer_diagram.png)

- **[The DDL module table](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/ddl.c#L27-L128)** -
  each peripheral holds its start and stop functions and the events it wants to
  hear about, so adding a sensor requires adding a row instead of editing code.
- **[The registration loop](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/ddl.c#L130-L153)** -
  goes over the modules once at startup, starting each module and signing it up
  on the event bus for every event it listed.
- **[Scheduler timing](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/app/scheduler/scheduler_config.h)** -
  the whole timing policy is one place: the 2 second cycle divided by the number
  of slots gives us its tick period.
- **[The tick handler](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/app/scheduler/scheduler_fsm.c#L15-L21)** -
  the timer callback only drops a tick event into the scheduler's own queue,
  so the real work still happens on the scheduler thread.
- **[The run state](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/app/scheduler/scheduler_fsm.c#L85-L124)** -
  each tick moves one slot forward and sends that slot its event, so every
  sensor gets its own turn and no two of them reach for the hardware at the same
  time.

## An example of a complete module (distance sensor)

The distance sensor driver is the first sensor module we completed, upon which
we based all the other sensors, so they all read the same way.

- **[Wire format](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/distance/distance_fsm.c#L23-L58)** -
  the request command and the layout of the reply, taken byte for byte from the
  sensor datasheet.
- **[The READ state](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/distance/distance_fsm.c#L239-L267)** -
  sends the command, asks for the reply, starts a timeout and returns
  immediately, so the thread is never left waiting on the sensor.
- **[Completion callbacks and retry](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/distance/distance_fsm.c#L146-L173)** -
  the UART and timer callbacks only send an event back into the FSM instead of
  doing the work themselves, which is what keeps things safe thread-wise and
  timing-wise.

## Hardware and protocol work

- **[Asynchronous UART](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/hal/uart/hal_uart.c#L108-L178)** -
  a read often comes back with only part of the reply, so the completion thread
  asks for the rest and calls the driver back only once the full buffer has arrived.
- **[Reading a register over I2C](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/hal/i2c/hal_i2c.c#L189-L216)** -
  sends the target register address and reads the answer back as a pair of messages.
- **[Reading a GPIO pin](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/hal/gpio/hal_gpio.c#L115-L138)** -
  a single call that returns the pin level right away.
- **[AM2302 single-wire bit decoding](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/temperature_humidity/temperature_humidity_fsm.c#L125-L148)** -
  the sensor sends each bit as a pulse on a single wire, and the code calculates
  whether the bit is a one or a zero by timing how long that pulse lasts.
- **[Event-driven GPS configuration](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/gps/gps_fsm.c#L247-L297)** -
  the configuration moves one step forward each time the previous step reports
  back.
- **[PCA9685 PWM frequency](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/servo/servo_fsm.c#L79-L113)** -
  changing the PWM frequency means putting the chip to sleep, writing the new 
  value and waking it again, in exactly that order, as the datasheet requires.
- **[Servo angle to PWM](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/servo/servo_fsm.c#L202-L241)** -
  turns a requested angle into a pulse width using the measured range of the
  servos, and rejects angles the arm can't physically reach.

## Sensor data aggregation

- **[The shared frame](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/ddl/ddl_frame.h)** -
  one struct with a field per sensor, with the padding spelled out so its size
  and layout stay the same during transmition.
- **[The broadcaster snapshot](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/src/app/broadcaster/broadcaster_fsm.c#L7-L50)** -
  copies the live readings into a second copy once per scheduler cycle, so the
  app always gets a matching set of values instead of catching some sensors
  mid-update.

---

## Hardware tests

The measurement programs live on the [`measurements`](https://github.com/emilglater/SnipeIt/tree/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw)
branch, because they need an extra instrumentation module and code that we
deliberately keep out of the production build.

- **[The test build](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw/Makefile#L11-L12)** -
  builds against every production source except `main.c`, so the numbers come
  from the code that actually ships rather than from a copy written for the test.

### Test 1 — Active Object dispatch latency

- **[Method 1](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw/test1_event_bus_latency.c#L1-L11)** -
  measures how long an event waits between entering a queue and being picked
  up, with the whole system running, so the numbers include the real competition
  between threads.

### Test 2 — GPS static position scatter

- **[Method 2](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw/test2_gps_scatter.c#L1-L15)** -
  records how far the reported position drifts while the unit sits still.

### Test 3 — Pan axis accuracy, repeatability and hysteresis

- **[Method 3](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw/test3_turret_accuracy.c#L1-L25)** -
  uses the compass as the angle reference as it turns together with the arm.

### Instrumentation

- **[The timing rule](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/src/util/trace/util_trace.h#L30-L39)** -
  the send has to be recorded before the event goes into the queue, otherwise
  the thread can pick it up first and every later pair of timestamps comes out
  shifted by one.
- **[The two added hooks](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw/hooks.diff)** -
  measuring the framework needed two extra calls in `active_object.c`.
- **[From raw data to the reported numbers](https://github.com/emilglater/SnipeIt/blob/1833783d961e80b9621a4a0719fc6c863d8f94be/tests_hw/reduce.py#L22-L31)** -
  turns the CSVs into the average, the 95th percentile and the worst case that
  can later be used for analysis.

## Host unit tests

- **[Active Object framework](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/test/active_object/test_active_object.c)** -
  covers thread startup, initialization and sending events, to test the active
  object logic.
- **[Distance FSM with the hardware mocked out](https://github.com/emilglater/SnipeIt/blob/1667748340fe89da5c506ace6532c7682206b78d/test/distance/test_distance.c)** -
  CMock stands in for the UART, the timer and the FSM, and the test keeps hold
  of the callbacks the driver hands over so it can trigger them itself, which is
  how the state machine gets tested with no hardware attached.