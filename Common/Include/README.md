The header the driver includes: `Driver.cpp` does `#include <AdapterOption.h>` for the adapter
selection it hands to IddCx. It is deliberately a separate header, marked `inline` so it can
be included in more than one translation unit.
