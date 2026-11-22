/*
 * Milestone FO: the layer routing matrix. Rows are production layers,
 * columns are destinations. These checks pin the FO.0 table — including the
 * cells that do not exist (speaker notes on the projector) — and the
 * settings/RPC codec that every door quotes.
 */

#include <cstdio>
#include <string>

#include <boost/json.hpp>

#include "openshow/domain/outputrouting.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::domain::OutputRouting;
using openshow::domain::OutputTarget;
using openshow::domain::RoutingLayer;
using openshow::domain::kAllRoutingLayers;
using openshow::domain::outputTargetFromName;
using openshow::domain::outputTargetName;
using openshow::domain::outputRoutingFromJson;
using openshow::domain::outputRoutingToJson;
using openshow::domain::routingLayerBit;
using openshow::domain::routingLayerFromName;
using openshow::domain::routingLayerName;
using openshow::domain::routingLayerSupported;
using openshow::domain::routingMaskNames;
using openshow::domain::validOutputRouting;
using openshow::domain::validRoutingMask;

void exerciseVocabulary() {
    for (OutputTarget target : {OutputTarget::Audience, OutputTarget::Stage,
                                OutputTarget::Broadcast}) {
        OutputTarget parsed = OutputTarget::Audience;
        check(outputTargetFromName(outputTargetName(target), &parsed) && parsed == target,
              "every destination name round-trips");
    }
    OutputTarget unknown = OutputTarget::Audience;
    check(!outputTargetFromName("ndi", &unknown),
          "NDI is a transport, not a destination name");
    for (std::size_t i = 0; i < openshow::domain::kRoutingLayerCount; ++i) {
        const RoutingLayer layer = static_cast<RoutingLayer>(i);
        RoutingLayer parsed = RoutingLayer::Background;
        check(routingLayerFromName(routingLayerName(layer), &parsed) && parsed == layer,
              "every production layer name round-trips");
        check(routingLayerBit(layer) != 0u, "every layer owns a bit");
    }
    RoutingLayer parsed = RoutingLayer::Background;
    check(!routingLayerFromName("clock", &parsed) && !routingLayerFromName("aux", &parsed),
          "the diagnostics clock and the demo aux badge are not routable");

    check(routingLayerSupported(OutputTarget::Audience, RoutingLayer::Background) &&
              routingLayerSupported(OutputTarget::Audience, RoutingLayer::Images) &&
              routingLayerSupported(OutputTarget::Audience, RoutingLayer::Words),
          "the audience receives background, images and words");
    check(routingLayerSupported(OutputTarget::Broadcast, RoutingLayer::Background) &&
              routingLayerSupported(OutputTarget::Broadcast, RoutingLayer::Images) &&
              routingLayerSupported(OutputTarget::Broadcast, RoutingLayer::Words),
          "broadcast receives background, images and words too (words-only is a default)");
    check(routingLayerSupported(OutputTarget::Stage, RoutingLayer::Background) &&
              routingLayerSupported(OutputTarget::Stage, RoutingLayer::Images) &&
              routingLayerSupported(OutputTarget::Stage, RoutingLayer::Words),
          "the stage receives background, images and words");
    check(!routingLayerSupported(OutputTarget::Audience, RoutingLayer::Next) &&
              !routingLayerSupported(OutputTarget::Broadcast, RoutingLayer::Next),
          "the next cue is a stage-only cell");
    check(!routingLayerSupported(OutputTarget::Audience, RoutingLayer::Notes) &&
              !routingLayerSupported(OutputTarget::Broadcast, RoutingLayer::Notes),
          "speaker notes can never route to the audience or the stream");
    check(routingLayerSupported(OutputTarget::Stage, RoutingLayer::Next) &&
              routingLayerSupported(OutputTarget::Stage, RoutingLayer::Notes),
          "the stage offers next cue and notes");
}

void exerciseDefaults() {
    const OutputRouting routing;
    check(!routing.isEnabled(OutputTarget::Audience) &&
              !routing.isEnabled(OutputTarget::Stage) &&
              !routing.isEnabled(OutputTarget::Broadcast),
          "every destination starts disabled");
    check(routing.mask(OutputTarget::Audience) ==
              (routingLayerBit(RoutingLayer::Background) |
               routingLayerBit(RoutingLayer::Images) |
               routingLayerBit(RoutingLayer::Words)),
          "the audience default is background, images and words");
    check(routing.mask(OutputTarget::Stage) ==
              (routingLayerBit(RoutingLayer::Words) |
               routingLayerBit(RoutingLayer::Next)),
          "the stage default is words and the next cue");
    check(routing.mask(OutputTarget::Broadcast) == routingLayerBit(RoutingLayer::Words),
          "the broadcast default is words only (alpha everywhere else)");
    std::string error;
    check(validOutputRouting(routing, &error), "the default routing is valid");
}

void exerciseValidation() {
    std::string error;
    check(!validRoutingMask(OutputTarget::Audience, kAllRoutingLayers | (1u << 10), &error),
          "a bit this build does not define is refused");
    error.clear();
    check(!validRoutingMask(OutputTarget::Audience, routingLayerBit(RoutingLayer::Next),
                            &error) &&
              error.find("next") != std::string::npos,
          "the next cue on the audience is refused naming the layer");
    error.clear();
    check(!validRoutingMask(OutputTarget::Broadcast, routingLayerBit(RoutingLayer::Notes),
                            &error) &&
              error.find("notes") != std::string::npos &&
              error.find("broadcast") != std::string::npos,
          "notes on the broadcast are refused naming layer and destination");
    check(validRoutingMask(OutputTarget::Stage, kAllRoutingLayers, &error),
          "every layer is legal on the stage");
    check(validRoutingMask(OutputTarget::Audience, 0u, &error),
          "an empty audience mask is legal (a black/unpainted output)");
    check(validRoutingMask(OutputTarget::Broadcast, 0u, &error),
          "an empty broadcast mask is legal (fully transparent)");
}

void exerciseCodec() {
    std::string error;
    OutputRouting routing;
    routing.setEnabled(OutputTarget::Stage, true);
    routing.setMask(OutputTarget::Broadcast,
                    routingLayerBit(RoutingLayer::Background) |
                        routingLayerBit(RoutingLayer::Words));
    const boost::json::object json = outputRoutingToJson(routing);
    check(json.at("audience").as_object().at("enabled").as_bool() == false,
          "the codec writes every column's enablement");
    check(json.at("broadcast").as_object().at("layers").as_array().size() == 2,
          "the codec writes the chosen layers");
    check(json.at("stage").as_object().at("layers").as_array()[1].as_string() == "next",
          "the codec writes layers in bit order");
    const std::string wire = boost::json::serialize(json);
    const boost::json::object again = outputRoutingToJson(routing);
    check(boost::json::serialize(again) == wire, "the routing codec is deterministic");

    OutputRouting parsed;
    check(outputRoutingFromJson(json, &parsed, &error) && parsed == routing,
          "a routing object round-trips");

    // Absent columns keep their role defaults and stay disabled.
    const boost::json::value onlyStage =
        boost::json::parse(R"({"stage":{"enabled":true,"layers":["words"]}})");
    OutputRouting defaults;
    check(outputRoutingFromJson(onlyStage, &defaults, &error) &&
              defaults.isEnabled(OutputTarget::Stage) &&
              defaults.mask(OutputTarget::Stage) == routingLayerBit(RoutingLayer::Words) &&
              !defaults.isEnabled(OutputTarget::Audience) &&
              !defaults.isEnabled(OutputTarget::Broadcast) &&
              defaults.mask(OutputTarget::Audience) ==
                  openshow::domain::defaultRoutingMask(OutputTarget::Audience),
          "an absent column keeps its default mask and stays disabled");
}

void exerciseCodecRefusals() {
    std::string error;
    OutputRouting out;
    const auto refused = [&](const char* text, const char* what) {
        const boost::json::value value = boost::json::parse(text);
        error.clear();
        const bool bad = !outputRoutingFromJson(value, &out, &error);
        check(bad, what);
        return error;
    };
    check(refused(R"({"audience":{"layers":["words"],"monitor":2}})",
                  "an unknown column member is refused")
              .find("monitor") != std::string::npos,
          "the refusal names the unknown field");
    check(!refused(R"({"audience":{"layers":["next"]}})",
                   "an illegal cell is refused by the codec")
               .empty(),
          "the illegal-cell refusal is a message");
    check(!refused(R"({"broadcast":{"layers":["notes"]}})",
                   "notes on the broadcast are refused by the codec")
               .empty(),
          "the notes refusal is a message");
    check(!refused(R"({"stage":{"layers":["words","words"]}})",
                   "a duplicated layer is refused")
               .empty(),
          "the duplicate-layer refusal is a message");
    check(!refused(R"({"stage":{"layers":["clock"]}})",
                   "the diagnostics clock is not a routable layer")
               .empty(),
          "the clock refusal is a message");
    check(!refused(R"({"stage":{"enabled":"yes"}})",
                   "a non-boolean enablement is refused")
               .empty(),
          "the enablement refusal is a message");
    check(!refused(R"({"ndi":{"layers":["words"]}})", "NDI is not a column name")
               .empty(),
          "NDI is rejected as an unknown column");
    check(!refused(R"({"audience":{"layers":[2]}})", "a non-string layer is refused")
               .empty(),
          "the layer-type refusal is a message");
}

void exerciseNames() {
    const std::uint32_t mask = routingLayerBit(RoutingLayer::Background) |
                               routingLayerBit(RoutingLayer::Words) |
                               routingLayerBit(RoutingLayer::Notes);
    const std::vector<std::string> names = routingMaskNames(mask);
    check(names.size() == 3 && names[0] == "background" && names[1] == "words" &&
              names[2] == "notes",
          "mask names are ordered bottom to top like the compose stack");
}

}  // namespace

int main() {
    exerciseVocabulary();
    exerciseDefaults();
    exerciseValidation();
    exerciseCodec();
    exerciseCodecRefusals();
    exerciseNames();
    if (gFailed) {
        std::fprintf(stderr, "output_routing_tests: FAILED\n");
        return 1;
    }
    std::printf("ok: layer routing defaults, legal cells and codec exercised\n");
    return 0;
}
