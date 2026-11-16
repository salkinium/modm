# DroneCAN DSDL Code Generator

Generates header-only C serialization code for the DroneCAN (UAVCAN v0) data
types. Select the namespaces of the [DroneCAN DSDL](https://github.com/dronecan/DSDL)
with the `modm:dronecan:namespaces` option and add your own root namespace
folders with `modm:dronecan:sources`. The `modm:dronecan:types` option limits
the code to some types and their dependencies.

The API of the generated code follows the dronecan_dsdlc generator used by
ArduPilot. For `uavcan.protocol.NodeStatus`:

- `#include <uavcan.protocol.NodeStatus.h>`
- `struct uavcan_protocol_NodeStatus` with the fields, dynamic arrays as
  `struct { uint8_t len; T data[N]; }`, unions with a `union_tag`.
- `UAVCAN_PROTOCOL_NODESTATUS_ID`, `_SIGNATURE`, `_MAX_SIZE` and the constants.
- `uint32_t uavcan_protocol_NodeStatus_encode(const struct uavcan_protocol_NodeStatus*, uint8_t* buffer)`
  returns the payload length.
- `bool uavcan_protocol_NodeStatus_decode(const CanardRxTransfer*, struct uavcan_protocol_NodeStatus*)`
  returns true if the payload is invalid.
- Services have `...Request` and `...Response` structs in `_req.h` and `_res.h`.
- In C++, a `modm::dronecan::Type<uavcan_protocol_NodeStatus>` specialization
  with `id`, `signature`, `max_size`, `encode()` and `decode()`, and the
  `Request` or `Response` type of a service, for the typed helpers of the
  `modm:communication:cyphal` module.

The code packs the bits with the serialization functions of the DroneCAN fork
of libcanard, whose header is renamed to `<dronecan/canard.h>`. The transport
is implemented by libcanard v5, see the `modm:communication:cyphal` module.

The generator is the `modm_tools.dronecan_dsdl` command line tool, which
parses the DSDL with pydronecan and renders the code with jinja2:
`pip install dronecan jinja2`. The `modm:build:scons` module calls it during
the build. Other build systems can call it with the DSDL that this module
copies to `modm/ext/dronecan/dsdl`:

```sh
python3 -m modm_tools.dronecan_dsdl -O generated/dronecan \
    --build uavcan.protocol.NodeStatus modm/ext/dronecan/dsdl/uavcan
```

`--list` prints the files it would write, so that a build system can declare
them as outputs.
