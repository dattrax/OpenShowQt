#include "Layer.h"

namespace openshow::worker {

std::string_view layerName(LayerId id) {
    switch (id) {
    case LayerId::kBackground:
        return "background";
    case LayerId::kImages:
        return "images";
    case LayerId::kWords:
        return "words";
    case LayerId::kAux:
        return "aux";
    case LayerId::kClock:
        return "clock";
    case LayerId::kCount:
        break;
    }
    return {};
}

bool layerIdFromName(std::string_view name, LayerId* out) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        if (layerName(id) == name) {
            if (out) {
                *out = id;
            }
            return true;
        }
    }
    return false;
}

void LayerStack::add(std::unique_ptr<RenderLayer> layer) {
    if (!layer) {
        return;
    }
    fEnabledMask |= layerBit(layer->id());
    fLayers.push_back(std::move(layer));
}

void LayerStack::setEnabled(LayerId id, bool enabled) {
    if (enabled) {
        fEnabledMask |= layerBit(id);
    } else {
        fEnabledMask &= ~layerBit(id);
    }
}

bool LayerStack::isEnabled(LayerId id) const {
    return (fEnabledMask & layerBit(id)) != 0;
}

bool LayerStack::applyOverride(std::string_view name, bool enabled) {
    LayerId id;
    if (!layerIdFromName(name, &id)) {
        return false;
    }
    setEnabled(id, enabled);
    return true;
}

void LayerStack::draw(const FrameContext& ctx) const {
    for (const std::unique_ptr<RenderLayer>& layer : fLayers) {
        if ((fEnabledMask & layerBit(layer->id())) == 0) {
            // Functionally off: nothing is dispatched to Skia at all.
            continue;
        }
        layer->draw(ctx);
    }
}

}  // namespace openshow::worker
