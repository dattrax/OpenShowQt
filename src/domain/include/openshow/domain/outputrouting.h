/*
 * Layer routing (milestone FO, task FO.1d/FO.4b): which production layers
 * each destination receives.
 *
 * This is MACHINE state, not service state: it names this machine's display
 * and network routes, so it never travels inside a portable theme snapshot
 * or a service package. The type lives in the domain because the rules are
 * a contract every door shares — the settings file, the worker RPC, the
 * editor's matrix and the compositor's mask — and two libraries parsing
 * their own copy of "which layers may go to NDI" is how a projector gets
 * speaker notes.
 *
 * Rows are layers, columns are destinations (FO.0 §5). Background, Images
 * and Words are selectable on ALL THREE destinations; Next cue and Notes
 * exist only on the stage. The demo Aux badge and the diagnostics Clock are
 * not routable at all, so they have no name here.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace openshow::domain {

// The production layers a destination may receive. Deliberately NOT the
// renderer's LayerId enum: that one also holds the demo Aux badge and the
// diagnostics Clock, which are not production features and must never be
// offered to an operator as routable.
enum class RoutingLayer : std::uint8_t {
    Background = 0,  // solid colour, gradient, image or video behind everything
    Images,          // authored picture items
    Words,           // lyrics and text boxes
    Next,            // the acknowledged next cue (stage only)
    Notes,           // speaker notes (stage only; data source arrives with FO.5b)
    kCount,
};

inline constexpr std::size_t kRoutingLayerCount =
    static_cast<std::size_t>(RoutingLayer::kCount);

inline constexpr std::uint32_t routingLayerBit(RoutingLayer layer) {
    return 1u << static_cast<std::uint32_t>(layer);
}

inline constexpr std::uint32_t kAllRoutingLayers = (1u << kRoutingLayerCount) - 1u;

// The wire/settings spelling of a layer, one word, lower case.
inline const char* routingLayerName(RoutingLayer layer) {
    switch (layer) {
        case RoutingLayer::Background:
            return "background";
        case RoutingLayer::Images:
            return "images";
        case RoutingLayer::Words:
            return "words";
        case RoutingLayer::Next:
            return "next";
        case RoutingLayer::Notes:
            return "notes";
        case RoutingLayer::kCount:
            break;
    }
    return "words";
}

inline bool routingLayerFromName(const std::string& name, RoutingLayer* layer) {
    if (name == "background") {
        *layer = RoutingLayer::Background;
        return true;
    }
    if (name == "images") {
        *layer = RoutingLayer::Images;
        return true;
    }
    if (name == "words") {
        *layer = RoutingLayer::Words;
        return true;
    }
    if (name == "next") {
        *layer = RoutingLayer::Next;
        return true;
    }
    if (name == "notes") {
        *layer = RoutingLayer::Notes;
        return true;
    }
    return false;
}

// The destinations. The audience is the operator's chosen display; stage is
// a separate HDMI window; broadcast is the NDI transport. NDI is a transport
// for `broadcast`, never its own content or role name.
enum class OutputTarget : std::uint8_t { Audience = 0, Stage = 1, Broadcast = 2 };

inline constexpr std::size_t kOutputTargetCount = 3;

inline const char* outputTargetName(OutputTarget target) {
    switch (target) {
        case OutputTarget::Stage:
            return "stage";
        case OutputTarget::Broadcast:
            return "broadcast";
        case OutputTarget::Audience:
            break;
    }
    return "audience";
}

inline bool outputTargetFromName(const std::string& name, OutputTarget* target) {
    if (name == "audience") {
        *target = OutputTarget::Audience;
        return true;
    }
    if (name == "stage") {
        *target = OutputTarget::Stage;
        return true;
    }
    if (name == "broadcast") {
        *target = OutputTarget::Broadcast;
        return true;
    }
    return false;
}

// Which cells exist. Unsupported cells are visibly disabled in the editor
// and refused at every door, never accepted and ignored.
inline bool routingLayerSupported(OutputTarget target, RoutingLayer layer) {
    switch (layer) {
        case RoutingLayer::Background:
        case RoutingLayer::Images:
        case RoutingLayer::Words:
            return true;
        case RoutingLayer::Next:
        case RoutingLayer::Notes:
            return target == OutputTarget::Stage;
        case RoutingLayer::kCount:
            break;
    }
    return false;
}

// The FO.0 default table: audience gets the full picture, stage gets the
// words and the next cue, broadcast starts words-only (alpha everywhere
// else) until the operator asks for background/images.
inline std::uint32_t defaultRoutingMask(OutputTarget target) {
    switch (target) {
        case OutputTarget::Audience:
            return routingLayerBit(RoutingLayer::Background) |
                   routingLayerBit(RoutingLayer::Images) |
                   routingLayerBit(RoutingLayer::Words);
        case OutputTarget::Stage:
            return routingLayerBit(RoutingLayer::Words) |
                   routingLayerBit(RoutingLayer::Next);
        case OutputTarget::Broadcast:
            break;
    }
    return routingLayerBit(RoutingLayer::Words);
}

// One machine's routing answer. `enabled` is SESSION enablement, deliberately
// separate from the desired mask: editing a cell changes no HDMI mask, and a
// saved enabled destination is never silently re-enabled after a restart.
// Defaults: everything disabled, every mask at its role default.
struct OutputRouting {
    bool enabled[kOutputTargetCount] = {false, false, false};
    std::uint32_t layers[kOutputTargetCount] = {
        defaultRoutingMask(OutputTarget::Audience),
        defaultRoutingMask(OutputTarget::Stage),
        defaultRoutingMask(OutputTarget::Broadcast),
    };

    bool operator==(const OutputRouting&) const = default;

    bool isEnabled(OutputTarget target) const {
        return enabled[static_cast<std::size_t>(target)];
    }
    std::uint32_t mask(OutputTarget target) const {
        return layers[static_cast<std::size_t>(target)];
    }
    void setMask(OutputTarget target, std::uint32_t mask) {
        layers[static_cast<std::size_t>(target)] = mask;
    }
    void setEnabled(OutputTarget target, bool on) {
        enabled[static_cast<std::size_t>(target)] = on;
    }
};

// The one semantic sentence about a mask: known bits only, and no stage-only
// layer on the audience or the broadcast. A mask bit this build does not
// define is refused rather than masked away, so a newer peer's intention is
// never silently dropped.
bool validRoutingMask(OutputTarget target, std::uint32_t mask, std::string* error);
bool validOutputRouting(const OutputRouting& routing, std::string* error);

// The settings/RPC spelling: `{"audience":{"enabled":bool,"layers":[...]},
// "stage":{...},"broadcast":{...}}`. Unknown members, duplicate layer names,
// unknown layers and illegal cells are refused naming themselves. Writing
// uses a fixed key order so two equal routings serialise byte-equal.
boost::json::object outputRoutingToJson(const OutputRouting& routing);
bool outputRoutingFromJson(const boost::json::value& value, OutputRouting* out,
                           std::string* error);

// The mask as its ordered names (bit order, bottom to top), for the worker's
// layer toggles and for a settings file a person may read.
std::vector<std::string> routingMaskNames(std::uint32_t mask);

}  // namespace openshow::domain
