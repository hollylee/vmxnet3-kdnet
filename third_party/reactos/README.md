# ReactOS KDNET headers

`kdnetextensibility.h` and `kdnetshareddata.h` come from the ReactOS project,
`sdk/include/reactos/`, at commit `77d883a64d90219d6e4e5cc33b398b795d445e0d`.
Their license is MIT, and the copyright notices are kept in each file.

Local change: in `kdnetextensibility.h`, `#include <reactos/kdnetshareddata.h>`
was changed to `#include "kdnetshareddata.h"`.

These headers are not from Microsoft. The probe module (`src/probe`) exists to
check their layouts against real Windows builds before anything else depends on
them.
