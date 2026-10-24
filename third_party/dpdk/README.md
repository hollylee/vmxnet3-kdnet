# DPDK vmxnet3 reference definitions

`vmxnet3_defs.h` and `upt1_defs.h` are unmodified reference copies from
DPDK tag `v24.11`, under `drivers/net/vmxnet3/base/`:

- https://github.com/DPDK/dpdk/blob/v24.11/drivers/net/vmxnet3/base/vmxnet3_defs.h
- https://github.com/DPDK/dpdk/blob/v24.11/drivers/net/vmxnet3/base/upt1_defs.h

Copyright (C) 2007 VMware, Inc. All rights reserved.
License: BSD-3-Clause; the accompanying `LICENSE` is copied from DPDK's
`license/bsd-3-clause.txt` at the same tag. Retain the notice and license in
source and binary distributions.

The build uses the reduced Windows-type definitions in
`src/vmxnet3/vmxnet3_hw.h`. These reference copies are not build inputs.
The reduced header preserves the revision-1 layouts; later revision
extensions are omitted. Register and RX descriptor bit fields are represented
by masks rather than compiler bit fields.

Device sequencing was checked against the BSD-licensed `vmxnet3_ethdev.c`
and `vmxnet3_rxtx.c` from the same directory's parent at tag `v24.11`.
