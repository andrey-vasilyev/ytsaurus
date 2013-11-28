#include "shutdown.h"

#include <ytlib/shutdown.h>

#include <ytlib/driver/dispatcher.h>

#include <contrib/libs/pycxx/Objects.hxx>

namespace NYT {
namespace NPython {

///////////////////////////////////////////////////////////////////////////////

void RegisterShutdown()
{
    static bool registered = false;
    if (!registered) {
        registered = true;
        Py_AtExit(Shutdown);
    }
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NPython
} // namespace NYT

