#include "config.h"

namespace NYT::NTracing {

////////////////////////////////////////////////////////////////////////////////

TTracingConfig::TTracingConfig()
{
    RegisterParameter("send_baggage", SendBaggage)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTracing
