/*
 * The portable remembered look's codec (task PT.1).
 *
 * Shape only here: the semantic answers (colour shape, weight band, word
 * sets, effect records, placeable boxes, transition band, context names)
 * are the domain's existing sentences, quoted through
 * Documents::validThemeSnapshot so the catalog and a theme cannot drift
 * into two ideas of what a look may say.
 */

#include "openshow/domain/themesnapshot.h"

#include <cmath>
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

bool wantString(const boost::json::object& object, const char* name,
                std::size_t maxChars, std::string* out, const std::string& owner,
                std::string* error) {
    const boost::json::value* value = field(object, name);
    if (value == nullptr || !value->is_string()) {
        fail(error, owner + " wants a string '" + name + "'");
        return false;
    }
    std::string text(value->as_string());
    if (text.size() > maxChars) {
        fail(error, owner + " '" + name + "' is over " + std::to_string(maxChars) +
                        " characters");
        return false;
    }
    *out = std::move(text);
    return true;
}

bool wantNumber(const boost::json::object& object, const char* name, double* out,
                const std::string& owner, std::string* error) {
    const boost::json::value* value = field(object, name);
    if (value == nullptr || !value->is_number()) {
        fail(error, owner + " wants a number '" + name + "'");
        return false;
    }
    *out = value->to_number<double>();
    return true;
}

bool wantInt(const boost::json::object& object, const char* name, int* out,
             const std::string& owner, std::string* error) {
    double number = 0.0;
    if (!wantNumber(object, name, &number, owner, error)) {
        return false;
    }
    if (number != std::floor(number) || number < -2147483648.0 ||
        number > 2147483647.0) {
        fail(error, owner + " '" + name + "' must be a whole number");
        return false;
    }
    *out = static_cast<int>(number);
    return true;
}

bool wantBool(const boost::json::object& object, const char* name, bool* out,
              const std::string& owner, std::string* error) {
    const boost::json::value* value = field(object, name);
    if (value == nullptr || !value->is_bool()) {
        fail(error, owner + " wants a boolean '" + name + "'");
        return false;
    }
    *out = value->as_bool();
    return true;
}

boost::json::object boxJson(const BoxRect& box) {
    boost::json::object out;
    out.emplace("x", box.x);
    out.emplace("y", box.y);
    out.emplace("width", box.width);
    out.emplace("height", box.height);
    return out;
}

bool boxFromJson(const boost::json::value& value, BoxRect* out, const std::string& owner,
                 std::string* error) {
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object, {"x", "y", "width", "height"}, owner, error)) {
        return false;
    }
    BoxRect box;
    if (!wantNumber(object, "x", &box.x, owner, error) ||
        !wantNumber(object, "y", &box.y, owner, error) ||
        !wantNumber(object, "width", &box.width, owner, error) ||
        !wantNumber(object, "height", &box.height, owner, error)) {
        return false;
    }
    *out = box;
    return true;
}

boost::json::object strokeJson(const TextStroke& stroke) {
    boost::json::object out;
    out.emplace("enabled", stroke.enabled);
    out.emplace("color", stroke.color);
    out.emplace("width", stroke.width);
    return out;
}

bool strokeFromJson(const boost::json::value& value, TextStroke* out,
                    const std::string& owner, std::string* error) {
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object, {"enabled", "color", "width"}, owner, error)) {
        return false;
    }
    TextStroke stroke;
    if (!wantBool(object, "enabled", &stroke.enabled, owner, error) ||
        !wantString(object, "color", 32, &stroke.color, owner, error) ||
        !wantNumber(object, "width", &stroke.width, owner, error)) {
        return false;
    }
    *out = stroke;
    return true;
}

boost::json::object shadowJson(const TextShadow& shadow) {
    boost::json::object out;
    out.emplace("enabled", shadow.enabled);
    out.emplace("color", shadow.color);
    out.emplace("opacity", shadow.opacity);
    out.emplace("offsetX", shadow.offsetX);
    out.emplace("offsetY", shadow.offsetY);
    out.emplace("blurSigma", shadow.blurSigma);
    return out;
}

bool shadowFromJson(const boost::json::value& value, TextShadow* out,
                    const std::string& owner, std::string* error) {
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object,
                       {"enabled", "color", "opacity", "offsetX", "offsetY", "blurSigma"},
                       owner, error)) {
        return false;
    }
    TextShadow shadow;
    if (!wantBool(object, "enabled", &shadow.enabled, owner, error) ||
        !wantString(object, "color", 32, &shadow.color, owner, error) ||
        !wantNumber(object, "opacity", &shadow.opacity, owner, error) ||
        !wantNumber(object, "offsetX", &shadow.offsetX, owner, error) ||
        !wantNumber(object, "offsetY", &shadow.offsetY, owner, error) ||
        !wantNumber(object, "blurSigma", &shadow.blurSigma, owner, error)) {
        return false;
    }
    *out = shadow;
    return true;
}

boost::json::object transitionJson(const TransitionOptions& transition) {
    boost::json::object out;
    out.emplace("kind", transitionKindName(transition.kind));
    out.emplace("textDurationMs", transition.textDurationMs);
    out.emplace("mediaDurationMs", transition.mediaDurationMs);
    return out;
}

bool transitionFromJson(const boost::json::value& value, TransitionOptions* out,
                        const std::string& owner, std::string* error) {
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object, {"kind", "textDurationMs", "mediaDurationMs"}, owner,
                       error)) {
        return false;
    }
    std::string kind;
    if (!wantString(object, "kind", 16, &kind, owner, error)) {
        return false;
    }
    TransitionOptions transition;
    if (!transitionKindFromName(kind, &transition.kind)) {
        fail(error, owner + " 'kind' must be \"fade\" or \"cut\"");
        return false;
    }
    if (!wantInt(object, "textDurationMs", &transition.textDurationMs, owner, error) ||
        !wantInt(object, "mediaDurationMs", &transition.mediaDurationMs, owner, error)) {
        return false;
    }
    *out = transition;
    return true;
}

boost::json::object contextJson(const TextContext& context) {
    boost::json::object out;
    out.emplace("name", context.name);
    out.emplace("fontFamily", context.fontFamily.value_or(""));
    out.emplace("fontSize", context.fontSize.value_or(0.0));
    out.emplace("lineSpacing", context.lineSpacing.value_or(kDefaultLineSpacing));
    out.emplace("align", context.align.value_or("center"));
    out.emplace("verticalAlign", context.verticalAlign.value_or(kDefaultVerticalAlign));
    out.emplace("allCaps", context.allCaps.value_or(false));
    if (context.box.has_value()) {
        out.emplace("box", boxJson(*context.box));
    } else if (context.inset.has_value()) {
        out.emplace("inset", *context.inset);
    }
    out.emplace("transition",
                transitionJson(context.transition.value_or(TransitionOptions::defaults())));
    return out;
}

bool contextFromJson(const boost::json::value& value, TextContext* out,
                     const std::string& owner, std::string* error) {
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object,
                       {"name", "fontFamily", "fontSize", "lineSpacing", "align",
                        "box", "inset", "verticalAlign", "allCaps", "transition"},
                       owner, error)) {
        return false;
    }
    TextContext context;
    std::string family;
    double size = 0.0;
    double spacing = kDefaultLineSpacing;
    std::string align;
    std::string verticalAlign;
    bool allCaps = false;
    TransitionOptions transition;
    if (!wantString(object, "name", kMaxThemeSnapshotNameChars, &context.name, owner,
                    error) ||
        !wantString(object, "fontFamily", kMaxThemeSnapshotFamilyChars, &family, owner,
                    error) ||
        !wantNumber(object, "fontSize", &size, owner, error) ||
        !wantNumber(object, "lineSpacing", &spacing, owner, error) ||
        !wantString(object, "align", 16, &align, owner, error) ||
        !wantString(object, "verticalAlign", 16, &verticalAlign, owner, error) ||
        !wantBool(object, "allCaps", &allCaps, owner, error) ||
        !transitionFromJson(*field(object, "transition"), &transition, owner + " transition",
                            error)) {
        return false;
    }
    context.fontFamily = family;
    context.fontSize = size;
    context.lineSpacing = spacing;
    context.align = align;
    context.verticalAlign = verticalAlign;
    context.allCaps = allCaps;
    context.transition = transition;
    const boost::json::value* box = field(object, "box");
    const boost::json::value* inset = field(object, "inset");
    if (box != nullptr && inset != nullptr) {
        fail(error, owner + " carries both 'box' and 'inset'");
        return false;
    }
    if (box != nullptr) {
        BoxRect rect;
        if (!boxFromJson(*box, &rect, owner + " box", error)) {
            return false;
        }
        context.box = rect;
    } else if (inset != nullptr) {
        double side = 0.0;
        if (!inset->is_number()) {
            fail(error, owner + " 'inset' must be a number");
            return false;
        }
        side = inset->to_number<double>();
        if (!std::isfinite(side) || side < 0.0) {
            fail(error, owner + " 'inset' must be finite and >= 0");
            return false;
        }
        context.inset = side;
    }
    *out = context;
    return true;
}

boost::json::object profileJson(const OutputProfile& profile) {
    boost::json::object out;
    out.emplace("role", outputRoleName(profile.role));
    out.emplace("content", profile.content);
    if (profile.textColor.has_value()) {
        out.emplace("textColor", *profile.textColor);
    }
    if (profile.fontFamily.has_value()) {
        out.emplace("fontFamily", *profile.fontFamily);
    }
    if (profile.fontSize.has_value()) {
        out.emplace("fontSize", *profile.fontSize);
    }
    if (profile.lineSpacing.has_value()) {
        out.emplace("lineSpacing", *profile.lineSpacing);
    }
    if (profile.align.has_value()) {
        out.emplace("align", *profile.align);
    }
    if (profile.box.has_value()) {
        out.emplace("box", boxJson(*profile.box));
    }
    if (profile.inset.has_value()) {
        out.emplace("inset", *profile.inset);
    }
    if (profile.verticalAlign.has_value()) {
        out.emplace("verticalAlign", *profile.verticalAlign);
    }
    if (profile.allCaps.has_value()) {
        out.emplace("allCaps", *profile.allCaps);
    }
    if (profile.fontWeight.has_value()) {
        out.emplace("fontWeight", *profile.fontWeight);
    }
    if (profile.fontStyle.has_value()) {
        out.emplace("fontStyle", *profile.fontStyle);
    }
    if (profile.fontUnderline.has_value()) {
        out.emplace("fontUnderline", *profile.fontUnderline);
    }
    if (profile.fitMode.has_value()) {
        out.emplace("fitMode", *profile.fitMode);
    }
    if (profile.minFontSize.has_value()) {
        out.emplace("minFontSize", *profile.minFontSize);
    }
    if (profile.stroke.has_value()) {
        out.emplace("stroke", strokeJson(*profile.stroke));
    }
    if (profile.shadow.has_value()) {
        out.emplace("shadow", shadowJson(*profile.shadow));
    }
    return out;
}

bool profileFromJson(const boost::json::value& value, OutputProfile* out,
                     const std::string& owner, std::string* error) {
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object,
                       {"role", "content", "textColor", "fontFamily", "fontSize",
                        "lineSpacing", "align", "box", "inset", "verticalAlign",
                        "allCaps", "fontWeight", "fontStyle", "fontUnderline",
                        "fitMode", "minFontSize", "stroke", "shadow"},
                       owner, error)) {
        return false;
    }
    OutputProfile profile;
    std::string role;
    if (!wantString(object, "role", 16, &role, owner, error) ||
        !outputRoleFromName(role, &profile.role)) {
        fail(error, owner + " 'role' must be \"stage\" or \"broadcast\"");
        return false;
    }
    if (!wantString(object, "content", kMaxOutputContentChars, &profile.content, owner,
                    error)) {
        return false;
    }
    const auto optionalString = [&](const char* name, std::size_t maxChars,
                                    std::optional<std::string>* slot) {
        const boost::json::value* member = field(object, name);
        if (member == nullptr) {
            return true;
        }
        if (!member->is_string()) {
            fail(error, owner + " '" + name + "' must be a string");
            return false;
        }
        std::string text(member->as_string());
        if (text.size() > maxChars) {
            fail(error, owner + " '" + name + "' is over " + std::to_string(maxChars) +
                            " characters");
            return false;
        }
        *slot = std::move(text);
        return true;
    };
    const auto optionalNumber = [&](const char* name, std::optional<double>* slot) {
        const boost::json::value* member = field(object, name);
        if (member == nullptr) {
            return true;
        }
        if (!member->is_number()) {
            fail(error, owner + " '" + name + "' must be a number");
            return false;
        }
        *slot = member->to_number<double>();
        return true;
    };
    const auto optionalBool = [&](const char* name, std::optional<bool>* slot) {
        const boost::json::value* member = field(object, name);
        if (member == nullptr) {
            return true;
        }
        if (!member->is_bool()) {
            fail(error, owner + " '" + name + "' must be true or false");
            return false;
        }
        *slot = member->as_bool();
        return true;
    };
    if (!optionalString("textColor", 32, &profile.textColor) ||
        !optionalString("fontFamily", kMaxThemeSnapshotFamilyChars, &profile.fontFamily) ||
        !optionalNumber("fontSize", &profile.fontSize) ||
        !optionalNumber("lineSpacing", &profile.lineSpacing) ||
        !optionalString("align", 16, &profile.align) ||
        !optionalNumber("inset", &profile.inset) ||
        !optionalString("verticalAlign", 16, &profile.verticalAlign) ||
        !optionalBool("allCaps", &profile.allCaps) ||
        !optionalString("fontStyle", 16, &profile.fontStyle) ||
        !optionalBool("fontUnderline", &profile.fontUnderline) ||
        !optionalString("fitMode", 16, &profile.fitMode) ||
        !optionalNumber("minFontSize", &profile.minFontSize)) {
        return false;
    }
    {
        const boost::json::value* weight = field(object, "fontWeight");
        if (weight != nullptr) {
            if (!weight->is_number() || weight->to_number<double>() !=
                                            std::floor(weight->to_number<double>())) {
                fail(error, owner + " 'fontWeight' must be a whole number");
                return false;
            }
            profile.fontWeight = static_cast<int>(weight->to_number<double>());
        }
    }
    if (const boost::json::value* box = field(object, "box"); box != nullptr) {
        BoxRect rect;
        if (!boxFromJson(*box, &rect, owner + " box", error)) {
            return false;
        }
        profile.box = rect;
    }
    if (const boost::json::value* stroke = field(object, "stroke"); stroke != nullptr) {
        TextStroke record;
        if (!strokeFromJson(*stroke, &record, owner + " stroke", error)) {
            return false;
        }
        profile.stroke = record;
    }
    if (const boost::json::value* shadow = field(object, "shadow"); shadow != nullptr) {
        TextShadow record;
        if (!shadowFromJson(*shadow, &record, owner + " shadow", error)) {
            return false;
        }
        profile.shadow = record;
    }
    std::string why;
    if (!Documents::validOutputProfile(profile, &why)) {
        fail(error, owner + ": " + why);
        return false;
    }
    *out = std::move(profile);
    return true;
}

}  // namespace

boost::json::object themeSnapshotToJson(const ThemeSnapshot& snapshot) {
    boost::json::object out;
    out.emplace("name", snapshot.name);
    out.emplace("textColor", snapshot.textColor);
    out.emplace("fontFamily", snapshot.fontFamily);
    out.emplace("fontSize", snapshot.fontSize);
    out.emplace("align", snapshot.align);
    out.emplace("lineSpacing", snapshot.lineSpacing);
    out.emplace("fontWeight", snapshot.fontWeight);
    out.emplace("fontStyle", snapshot.fontStyle);
    out.emplace("fontUnderline", snapshot.fontUnderline);
    out.emplace("fitMode", snapshot.fitMode);
    out.emplace("minFontSize", snapshot.minFontSize);
    out.emplace("verticalAlign", snapshot.verticalAlign);
    out.emplace("allCaps", snapshot.allCaps);
    out.emplace("stroke", strokeJson(snapshot.stroke));
    out.emplace("shadow", shadowJson(snapshot.shadow));
    if (snapshot.placement.has_value()) {
        out.emplace("placement", boxJson(*snapshot.placement));
    }
    out.emplace("transition", transitionJson(snapshot.transition));
    boost::json::array contexts;
    for (const TextContext& context : snapshot.contexts) {
        contexts.push_back(contextJson(context));
    }
    out.emplace("contexts", std::move(contexts));
    boost::json::array profiles;
    for (const OutputProfile& profile : snapshot.profiles) {
        profiles.push_back(profileJson(profile));
    }
    out.emplace("profiles", std::move(profiles));
    return out;
}

bool themeSnapshotFromJson(const boost::json::value& value, ThemeSnapshot* out,
                           std::string* error) {
    const std::string owner = "theme snapshot";
    if (out == nullptr) {
        fail(error, owner + " needs a destination");
        return false;
    }
    if (!value.is_object()) {
        fail(error, owner + " must be an object");
        return false;
    }
    const boost::json::object& object = value.as_object();
    if (!noUnknownKeys(object,
                       {"name", "textColor", "fontFamily", "fontSize", "align",
                        "lineSpacing", "fontWeight", "fontStyle", "fontUnderline",
                        "fitMode", "minFontSize", "verticalAlign", "allCaps", "stroke",
                        "shadow", "placement", "transition", "contexts", "profiles"},
                       owner, error)) {
        return false;
    }
    ThemeSnapshot snapshot;
    if (!wantString(object, "name", kMaxThemeSnapshotNameChars, &snapshot.name, owner,
                    error) ||
        !wantString(object, "textColor", 32, &snapshot.textColor, owner, error) ||
        !wantString(object, "fontFamily", kMaxThemeSnapshotFamilyChars,
                    &snapshot.fontFamily, owner, error) ||
        !wantNumber(object, "fontSize", &snapshot.fontSize, owner, error) ||
        !wantString(object, "align", 16, &snapshot.align, owner, error) ||
        !wantNumber(object, "lineSpacing", &snapshot.lineSpacing, owner, error) ||
        !wantInt(object, "fontWeight", &snapshot.fontWeight, owner, error) ||
        !wantString(object, "fontStyle", 16, &snapshot.fontStyle, owner, error) ||
        !wantBool(object, "fontUnderline", &snapshot.fontUnderline, owner, error) ||
        !wantString(object, "fitMode", 16, &snapshot.fitMode, owner, error) ||
        !wantNumber(object, "minFontSize", &snapshot.minFontSize, owner, error) ||
        !wantString(object, "verticalAlign", 16, &snapshot.verticalAlign, owner, error) ||
        !wantBool(object, "allCaps", &snapshot.allCaps, owner, error) ||
        !strokeFromJson(*field(object, "stroke"), &snapshot.stroke, owner + " stroke",
                        error) ||
        !shadowFromJson(*field(object, "shadow"), &snapshot.shadow, owner + " shadow",
                        error) ||
        !transitionFromJson(*field(object, "transition"), &snapshot.transition,
                            owner + " transition", error)) {
        return false;
    }
    if (const boost::json::value* placement = field(object, "placement");
        placement != nullptr) {
        BoxRect rect;
        if (!boxFromJson(*placement, &rect, owner + " placement", error)) {
            return false;
        }
        snapshot.placement = rect;
    }
    const boost::json::value* contexts = field(object, "contexts");
    if (contexts == nullptr || !contexts->is_array()) {
        fail(error, owner + " wants an array 'contexts'");
        return false;
    }
    if (contexts->as_array().size() > kMaxThemeSnapshotContexts) {
        fail(error, owner + " carries more than " +
                        std::to_string(kMaxThemeSnapshotContexts) + " contexts");
        return false;
    }
    for (const boost::json::value& entry : contexts->as_array()) {
        TextContext context;
        if (!contextFromJson(entry, &context,
                             owner + " context " + std::to_string(snapshot.contexts.size()),
                             error)) {
            return false;
        }
        snapshot.contexts.push_back(std::move(context));
    }
    // A remembered look's output profiles (milestone FO): absent in a
    // snapshot written before FO, so those still load with none.
    if (const boost::json::value* profiles = field(object, "profiles");
        profiles != nullptr) {
        if (!profiles->is_array()) {
            fail(error, owner + " wants an array 'profiles'");
            return false;
        }
        if (profiles->as_array().size() > kMaxThemeOutputProfiles) {
            fail(error, owner + " carries more than " +
                            std::to_string(kMaxThemeOutputProfiles) + " output profiles");
            return false;
        }
        for (const boost::json::value& entry : profiles->as_array()) {
            OutputProfile profile;
            if (!profileFromJson(entry, &profile,
                                 owner + " profile " +
                                     std::to_string(snapshot.profiles.size()),
                                 error)) {
                return false;
            }
            snapshot.profiles.push_back(std::move(profile));
        }
    }
    std::string why;
    if (!Documents::validThemeSnapshot(snapshot, &why)) {
        fail(error, owner + ": " + why);
        return false;
    }
    *out = std::move(snapshot);
    return true;
}

}  // namespace openshow::domain
