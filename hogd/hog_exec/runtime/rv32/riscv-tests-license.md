# Borrowed RV32 Test Vector License Notice

Some RV32 conformance vectors in `rv32_borrowed_conformance_test.cc` are adapted
from the upstream `riscv-tests` ISA test sources:

- Repository: `https://github.com/riscv-software-src/riscv-tests`
- Local source checkout inspected: commit
  `1eb47d946c55f55cab8653c224c2993acc0276bd`
- Source paths: selected `isa/rv32ui/*.S` wrappers, their included
  `isa/rv64ui/*.S` bodies, and selected `isa/rv32um/*.S` tests.

What was adapted: operand/result values and edge-case expectations from the
RV32UI/RV32UM assembly tests were transcribed into C++ tables. No upstream
assembly source, ELF harness, linker script, or binary test artifact is vendored
into this repository.

The upstream repository carries the following BSD-style notice, retained here
for the adapted vectors.

Copyright (c) 2012-2015, The Regents of the University of California
(Regents). All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the Regents nor the names of its contributors may be used
   to endorse or promote products derived from this software without specific
   prior written permission.

IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT, SPECIAL,
INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS, ARISING OUT OF THE
USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF REGENTS HAS BEEN ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.

REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
THE SOFTWARE AND ACCOMPANYING DOCUMENTATION, IF ANY, PROVIDED HEREUNDER IS
PROVIDED "AS IS". REGENTS HAS NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT,
UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
