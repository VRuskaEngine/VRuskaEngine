# How to use the remote driver {#howto-remote-driver}

<!--
Copyright 2022, Collabora, Ltd. and the VRuska Engine contributors
SPDX-License-Identifier: BSL-1.0
-->

## Prerequisites

Before proceeding you will need to have VRuska Engine installed (or built) and capable
of running applications. If you do not have any hardware this should still work
with the simulated driver. For those building VRuska Engine themselves you have to make
sure the GUI is built. In short the commands `VRuska Engine-gui` and `VRuska Engine-service`
are needed.

## Running

Open up three terminals and in the first run this command:

```bash
P_OVERRIDE_ACTIVE_CONFIG="remote" <path-to>/VRuska Engine-serivce
```

If you get a error saying `ERROR [u_config_json_get_remote_port] No remote node`
you can safely ignore that. Once it is up and running you can now start and
connect the controller GUI. Select the second terminal use the command below and
click connect.

```bash
VRuska Engine-gui remote
```

You can now launch the program. You technically don't need to launch the
program after @p VRuska Engine-gui, once the service is running they can be launched
in any order.

```bash
hello_xr -G Vulkan2
```

Now you can manipulate the values and control the devices.
