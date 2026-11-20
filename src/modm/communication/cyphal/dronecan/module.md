# DroneCAN Node

DroneCAN (UAVCAN v0) on top of libcanard v5 and the code that the
`modm:dronecan` module generates from the DroneCAN DSDL. This module adds the
generated types of its services to the `modm:dronecan:types` option.


## Node

`modm::dronecan::Node<Can>` is a `modm::cyphal::Node<Can>` with the standard
services of the `uavcan.protocol` namespace that every DroneCAN node needs:

- `NodeStatus` once per second, with `setHealth()`, `setMode()` and
  `setVendorStatus()`.
- `GetNodeInfo` with the name, versions and unique ID of `NodeInfo`.
- Dynamic node-ID allocation with `dynamic_node_id.Allocation`, if the node
  is initialized without node-ID. `nodeId()` is 0 until a server allocated one.
- `param.GetSet` and `param.ExecuteOpcode` for the parameters, which the DroneCAN
  GUI tool, Mission Planner and ArduPilot can read and write.
- `RestartNode`, if a restart function is configured.

```cpp
#include <modm/communication/cyphal.hpp>
namespace dronecan = modm::dronecan;

dronecan::Parameter rate{"example.rate_hz", 10, 1, 100};    // integer with range
dronecan::Parameter gain{"example.gain", 0.5f, 0.f, 10.f};  // real with range
dronecan::Parameter enable{"example.enable", true};         // boolean
dronecan::Parameter* const parameters[]{&rate, &gain, &enable};

dronecan::Node<Fdcan1> node;

std::array<uint8_t, 16> unique_id{};
// e.g. the UID of the device, see modm::platform::getUniqueId()
node.initialize({.name = "io.modm.example", .unique_id = unique_id, .software_major = 1},
                {.node_id = 0 /* dynamic allocation */, .parameters = parameters,
                 .restart = [] { NVIC_SystemReset(); }});

while (true)
{
    node.update();
    if (node.nodeId() and timer.execute())
        dronecan::publish(&node.canard, measurement(gain.real()), transfer_id);
}
```

The services answer in `update()`, which must be called every few
milliseconds. It needs about 1.5 kB of stack for the GetNodeInfo and GetSet
responses, which are encoded on the stack. Use the typed helpers and the libcanard API on `node.canard` for
the data types of the application, see below.

A DroneCAN node keeps its node-ID: libcanard would move to a free node-ID when
another node uses the same one, which DroneCAN does not allow. Instead, the
frames of the other node are dropped and counted in `node.collisions()`.

### Parameters

`modm::dronecan::Parameter` holds an integer, real or boolean value with a
default, and a range for integers and reals. The GetSet service refuses values
outside of the range and answers with the current value. Parameters are
accessed by name or by their index in the `parameters` span, whose order must
therefore stay the same. The application reads them with `integer()`,
`real()` or `boolean()` and may change them with `set()`.

The ExecuteOpcode service saves the parameters to a `ParameterStorage`
(`OPCODE_SAVE`) or removes them from it and resets them to their defaults
(`OPCODE_ERASE`). `node.save()` and `node.erase()` do the same from the
application. `dronecan::loadParameters(parameters, storage)` loads the stored
values before the node is initialized, so that e.g. the node-ID can be a
parameter. Each
parameter is stored under a 32-bit hash of its name and type, so the order
may change between firmware versions, and parameters with the default value
are removed from the storage, so that a new default takes effect.

With the `modm:zephyr-zms` module, `modm::dronecan::ZmsStorage` stores the
parameters in a ZMS file system on the internal or an external flash:

```cpp
#include <modm/communication/cyphal/dronecan/zms.hpp>

zms_fs fs;
modm::zms::setup(fs, Flash::Size - 4 * 2048, 4);
zms_mount(&fs);
dronecan::ZmsStorage storage{fs};
dronecan::loadParameters(parameters, storage);

node.initialize(info, {.node_id = uint8_t(node_id.integer()),
                       .parameters = parameters, .storage = &storage});
```

Saving blocks `update()` while the flash is programmed or erased. On devices
with a single flash bank, all interrupts are masked during these operations,
and the CAN receive FIFO overflows if frames arrive meanwhile. Put the storage
into the second bank of dual-bank devices instead, where the CPU keeps running
and the CAN driver keeps receiving into its software queue.


## Typed Helpers

The `modm:dronecan` module generates a `modm::dronecan::Type<T>` specialization
for every data type, so that the helpers take the data type ID, signature and
size from it:

```cpp
#include <uavcan.equipment.air_data.StaticPressure.h>

// publishing increments the transfer-ID
uavcan_equipment_air_data_StaticPressure pressure{.static_pressure = 101325};
dronecan::publish(&node.canard, pressure, transfer_id);
// services
dronecan::request(&node.canard, server_id, uavcan_protocol_GetNodeInfoRequest{}, transfer_id);
dronecan::respond(&node.canard, client_id, request_transfer_id, info);

// subscribing to messages, requests (as server) and responses (as client)
dronecan::subscribe<uavcan_protocol_NodeStatus>(&node.canard, &subscription, &vtable);
// in the subscription callback
uavcan_protocol_NodeStatus status;
if (dronecan::decode(payload, status)) { /* valid */ }
modm::cyphal::release(subscription, payload);
```

Subscription callbacks must not call libcanard, so they should store what they
received and handle it in the main loop.

The `canard_v0_*()` functions can also be used directly with the generated C
functions and `modm::dronecan::crcSeed()` and `modm::dronecan::transfer()`.

libcanard cannot send anonymous frames, since Cyphal assigns node-IDs without
them. `dronecan::publishAnonymous(node, message, transfer_id)` sends a single
frame anonymous message, e.g. the requests of the dynamic node-ID allocation.
