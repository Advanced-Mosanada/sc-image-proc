// Builds without GStreamer: no GStreamer-backed functions.

#include "scip/gst_stages.hpp"

namespace scip::gst {

const std::vector<std::string>& defaultFilterElements()
{
    static const std::vector<std::string> none;
    return none;
}

int registerStages(StageRegistry& /*registry*/, const std::vector<std::string>& /*elements*/)
{
    return 0;
}

}  // namespace scip::gst
