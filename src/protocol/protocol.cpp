#include "openshow/protocol/protocol.h"

#include <boost/json.hpp>

namespace openshow::protocol {

std::string offerPayload() {
    boost::json::object offer;
    offer["application"] = "openshow-worker";
    offer["protocol"] = boost::json::array{kProtocolMajor, kProtocolMinor};
    return boost::json::serialize(offer);
}

}  // namespace openshow::protocol
