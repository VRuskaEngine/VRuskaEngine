# CI and Generated Stuff Readme

<!--
# Copyright 2018-2024, Collabora, Ltd. and the VRuska Engine contributors
#
# SPDX-License-Identifier: CC0-1.0
-->

We use the FreeDesktop
["CI Templates"](https://freedesktop.pages.freedesktop.org/ci-templates) to
maintain build containers using code in this repo, storing the images in GitLab
Registry. Our CI files (and some other files) are auto-generated from Jinja
templates and `config.yml`, using
[ci-fairy](https://freedesktop.pages.freedesktop.org/ci-templates/ci-fairy.html).
You can install it with:

<!-- do not break the following line, it is used in CI setup too, to make sure it works -->
```sh
pipx install git+https://gitlab.freedesktop.org/freedesktop/ci-templates@185ede0e9b9b1924b92306ab8b882a6294e92613
```

On Windows you will also need to have GNU make and busybox installed, such as with:

```pwsh
scoop install make busybox
```

To re-generate files, from the root directory, run:

```sh
make -f .gitlab-ci/ci-scripts.mk
```

If you really want to force rebuilding, you can build the clean target first:

```sh
make -f .gitlab-ci/ci-scripts.mk clean all
```
