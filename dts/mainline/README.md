# The Tensor G5's device trees in mainline Linux

These six files are copied unchanged from Linux, `arch/arm64/boot/dts/google/`, at commit
[`18686b294c6f`](https://github.com/torvalds/linux/commit/18686b294c6f6ba778cba66eccf01b9fcdf48988) (2026-09-29, the
directory as merged for Linux 7.4: Douglas Anderson's and Peter Griffin's series, "arm64: dts: google: Add initial dts for
frankel/blazer/mustang", [`54edd8698bb1`](https://github.com/torvalds/linux/commit/54edd8698bb1)).

Each file is dual-licensed by its authors, `GPL-2.0 OR BSD-3-Clause` (the Makefile `GPL-2.0+ OR BSD-3-Clause`). This
repository takes them under the **BSD-3-Clause** licence, below, with Google's copyright notices kept in each file.

They are the reference this port's facts are checked against (README.md, "The hardware"), the cleanest description there is
of the SoC: the cores and their MPIDRs, the GIC, the timer's interrupts, the console UART, the ramoops region. They are not
built here; the shim reads the device tree the phone's boot loader hands it, which is Google's own, far larger one.

```
Copyright 2021-2025 Google LLC

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
