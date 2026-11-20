# Cyphal/CAN and DroneCAN Node

Connects a modm CAN driver to [libcanard](https://github.com/OpenCyphal/libcanard),
which implements Cyphal/CAN v1.1 and v1.0 and the legacy UAVCAN v0/DroneCAN.

`modm::cyphal::Node<Can>` owns a `canard_t`, a private o1heap arena for the
memory of libcanard, the receive loop and the acceptance filters. Use the
libcanard C API on `node.canard` to publish and subscribe, and the
`modm:nunavut` module to generate the serialization code of the data types.

```cpp
#include <modm/communication/cyphal.hpp>
#include <uavcan/node/Heartbeat_1_0.h>

modm::cyphal::Node<Fdcan1> node;

Fdcan1::connect<GpioB8::Rx, GpioB9::Tx>();
Fdcan1::initialize<Board::SystemClock, 500_kbps, 1_pct, 2_Mbps>(9);
node.initialize(42, /* unique seed */ 0x1234, {.fd = true});

while (true)
{
    node.update();
    if (timer.execute())
    {
        uavcan_node_Heartbeat_1_0 heartbeat{.uptime = uptime++};
        uint8_t buffer[uavcan_node_Heartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
        size_t size = sizeof(buffer);
        uavcan_node_Heartbeat_1_0_serialize_(&heartbeat, buffer, &size);
        canard_publish_13b(&node.canard, modm::cyphal::now() + 1'000'000,
                           CANARD_IFACE_BITMAP_ALL, canard_prio_nominal,
                           uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, transfer_id++,
                           {{size, buffer}, nullptr}, nullptr);
    }
}
```

Subscription callbacks receive the payload and must free it with
`modm::cyphal::release(subscription, payload)`.

For CAN FD, set `modm:architecture:can:message.buffer` to 64 and initialize the
CAN driver with a data bitrate.

## Memory

libcanard allocates memory for every queued frame, received transfer and
remote node, so the bus traffic decides how much it needs. By default each
node has a private o1heap arena of 4096 bytes (`Node<Can, ArenaSize>`), so that
a busy bus cannot exhaust the application heap. The allocations take constant
time, and an arena of twice the peak usage never fails due to fragmentation.
Use `node.heapDiagnostics()` to find the peak usage and size the arena.

With `Node<Can, 0>` libcanard uses `malloc()` and `free()` of the application
heap instead, which needs the `modm:platform:heap` module.


## CAN Driver Configuration

libcanard keeps its transmit queue sorted by priority and drops frames whose
deadline has passed. Frames that are already in the queues of the CAN driver
cannot be dropped anymore, so keep the driver queues short: for the STM32
FDCAN disable the software buffer (`buffer.tx = 0`) and use the priority mode
of the hardware queue (`tx_hw_queue_mode = Priority`). The bxCAN always has a
software buffer, which is sorted by priority as well.

libcanard configures the extended acceptance filters of the STM32 FDCAN and
bxCAN drivers, which needs one CAN controller per node: for several nodes on
one controller initialize them with `{.filters = false}` and configure a filter
that accepts all extended frames. On the bxCAN, the extended filters use all
filter banks of the instance, so they replace the filters set with `CanFilter`.

libcanard treats a received frame with its own node-ID as a node-ID collision,
so a node with a CAN controller in loopback mode must drop the echoes of its
own frames with `{.loopback = true}`. Several nodes on one controller read
the driver once and pass each frame to all nodes with `ingest()`.


## DroneCAN

libcanard also sends and receives DroneCAN (UAVCAN v0) transfers with the
`canard_v0_*()` functions, always as Classic CAN frames. The
`modm:communication:cyphal:dronecan` module adds typed helpers for the
generated DroneCAN data types and a node with the standard services.
