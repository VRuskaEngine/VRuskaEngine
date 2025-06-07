# Metrics collecting {#metrics}

<!--
Copyright 2022, Collabora, Ltd. and the VRuska Engine contributors
SPDX-License-Identifier: BSL-1.0
-->

## Requirements

VRuska Engine comes with it's own metrics collection and is built by default. No action
is needed by the developer to build the VRuska Engine sides of things. You will need to
checkout the [metrics repo][] to get the tools to analyse the resulting output
from VRuska Engine.

## Running

Launch the service with the env variable `XRT_METRICS_FILE` set.

```bash
XRT_METRICS_FILE=/path/to/file.protobuf VRuska Engine-service
```

After VRuska Engine has finished running run the tool in the [metrics repo][], follow
the instructions in the [README.md][] file inside of that repo, there are more
instructions there.

[metrics repo]: https://gitlab.freedesktop.org/VRuska Engine/utilities/metrics
[README.md]: https://gitlab.freedesktop.org/VRuska Engine/utilities/metrics/-/blob/main/README.md
