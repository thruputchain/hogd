# Borrowed RV32 Test Vector License Notice

Some RV32 conformance vectors in `rv32_borrowed_conformance_test.cc` are adapted
from the `riscv-tests` test sources vendored by PolkaVM:

- Repository: `git@github.com:paritytech/polkavm.git`
- Commit: `bd2d14ed105467fd158b4744fd92f7045d288bb8`
- Source path: `guest-programs/riscv-tests/tests`

PolkaVM itself is licensed under MIT OR Apache-2.0 at the repository root. The
borrowed vectors are not adapted from PolkaVM-specific runtime code; they come
from the vendored `riscv-tests` subtree, whose assembly sources reference the
subtree-local `tests/LICENSE`. That subtree carries the following BSD-style
notice, retained here for the adapted vectors.

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
