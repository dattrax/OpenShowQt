/* Layer routing's one implementation (milestone FO).
 *
 * The rules here are quoted by the settings file, the worker RPC, the
 * editor's matrix and the compositor's mask, so an illegal cell costs the
 * same words at every door.
 */

#include "openshow/domain/outputrouting.h"

#include <initializer_list>

namespace openshow::domain {

namespace {

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

const boost::json::value* field(const boost::json::object& object, const char* name) {
    const auto entry = object.find(name);
    return entry == object.end() ? nullptr : &entry->value();
}

bool noUnknownKeys(const boost::json::object& object,
                   std::initializer_list<const char*> allowed, const std::string& owner,
                   std::string* error) {
    for (const auto& entry : object) {
        bool known = false;
        for (const char* name : allowed) {
            if (entry.key() == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            fail(error, owner + " has no field '" + std::string(entry.key()) + "'");
            return false;
        }
    }
    return true;
}

}  // namespace

bool validRoutingMask(OutputTarget target, std::uint32_t mask, std::string* error) {
    const std::uint32_t unknown = mask & ~kAllRoutingLayers;
    if (unknown != 0u) {
        fail(error, std::string(outputTargetName(target)) + " layer mask names bit " +
                        std::to_string(unknown) +
                        ", which is not a production layer this build can draw");
        return false;
    }
    for (std::size_t i = 0; i < kRoutingLayerCount; ++i) {
        const RoutingLayer layer = static_cast<RoutingLayer>(i);
        if ((mask & routingLayerBit(layer)) != 0u && !routingLayerSupported(target, layer)) {
            fail(error, std::string("the ") + outputTargetName(target) +
                            " destination cannot receive the '" + routingLayerName(layer) +
                            "' layer; it is a stage-only layer");
            return false;
        }
    }
    return true;
}

bool validOutputRouting(const OutputRouting& routing, std::string* error) {
    for (std::size_t i = 0; i < kOutputTargetCount; ++i) {
        const OutputTarget target = static_cast<OutputTarget>(i);
        if (!validRoutingMask(target, routing.layers[i], error)) {
            return false;
        }
    }
    return true;
}

boost::json::object outputRoutingToJson(const OutputRouting& routing) {
    boost::json::object out;
    for (std::size_t i = 0; i < kOutputTargetCount; ++i) {
        const OutputTarget target = static_cast<OutputTarget>(i);
        boost::json::object column;
        column.emplace("enabled", routing.enabled[i]);
        boost::json::array layers;
        for (std::size_t layer = 0; layer < kRoutingLayerCount; ++layer) {
            const RoutingLayer id = static_cast<RoutingLayer>(layer);
            if ((routing.layers[i] & routingLayerBit(id)) != 0u) {
                layers.emplace_back(routingLayerName(id));
            }
        }
        column.emplace("layers", std::move(layers));
        out.emplace(outputTargetName(target), std::move(column));
    }
    return out;
}

bool outputRoutingFromJson(const boost::json::value& value, OutputRouting* out,
                           std::string* error) {
    if (out == nullptr) {
        fail(error, "routing needs a destination");
        return false;
    }
    if (!value.is_object()) {
        fail(error, "routing must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object, {"audience", "stage", "broadcast"}, "routing", error)) {
        return false;
    }
    OutputRouting routing;
    for (std::size_t i = 0; i < kOutputTargetCount; ++i) {
        const OutputTarget target = static_cast<OutputTarget>(i);
        const std::string owner = std::string("routing ") + outputTargetName(target);
        const boost::json::value* columnValue = field(object, outputTargetName(target));
        if (columnValue == nullptr) {
            continue;  // absent column keeps its role default, disabled
        }
        if (!columnValue->is_object()) {
            fail(error, owner + " must be an object");
            return false;
        }
        const boost::json::object& column = columnValue->as_object();
        if (!noUnknownKeys(column, {"enabled", "layers"}, owner, error)) {
            return false;
        }
        if (const boost::json::value* enabled = field(column, "enabled");
            enabled != nullptr) {
            if (!enabled->is_bool()) {
                fail(error, owner + " 'enabled' must be true or false");
                return false;
            }
            routing.enabled[i] = enabled->as_bool();
        }
        if (const boost::json::value* layers = field(column, "layers");
            layers != nullptr) {
            if (!layers->is_array()) {
                fail(error, owner + " 'layers' must be an array");
                return false;
            }
            std::uint32_t mask = 0u;
            for (const boost::json::value& entry : layers->as_array()) {
                if (!entry.is_string()) {
                    fail(error, owner + " 'layers' holds a non-string");
                    return false;
                }
                RoutingLayer layer = RoutingLayer::Background;
                if (!routingLayerFromName(std::string(entry.as_string()), &layer)) {
                    fail(error, owner + " names layer '" +
                                    std::string(entry.as_string()) +
                                    "', which is not a production layer");
                    return false;
                }
                const std::uint32_t bit = routingLayerBit(layer);
                if ((mask & bit) != 0u) {
                    fail(error, owner + " names layer '" + routingLayerName(layer) +
                                    "' twice");
                    return false;
                }
                mask |= bit;
            }
            routing.layers[i] = mask;
        }
    }
    std::string why;
    if (!validOutputRouting(routing, &why)) {
        fail(error, why);
        return false;
    }
    *out = routing;
    return true;
}

std::vector<std::string> routingMaskNames(std::uint32_t mask) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kRoutingLayerCount; ++i) {
        const RoutingLayer layer = static_cast<RoutingLayer>(i);
        if ((mask & routingLayerBit(layer)) != 0u) {
            names.emplace_back(routingLayerName(layer));
        }
    }
    return names;
}

}  // namespace openshow::domain
