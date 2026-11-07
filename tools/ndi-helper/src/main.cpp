// openshow-ndi-helper entry point.
//
// Server mode (the default, no arguments) reads commands from stdin, or from
// --input FILE, and writes protocol objects to stdout. --probe and --list are
// one-shot conveniences for setup and diagnostics; they print a single JSON
// object and exit.
//
// The backend is the fake colour-bar source unless --fake is passed or the
// helper was built without the NDI SDK. The protocol layer never mentions NDI.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "backend.h"
#include "fake_backend.h"
#include "json.h"
#include "protocol.h"

#ifdef OPENSHOW_NDI_HELPER_WITH_NDI
#include "ndi_backend.h"
#endif

namespace {

using openshow::ndi::Backend;

void printUsage(std::ostream& out) {
    out << "openshow-ndi-helper " << openshow::ndi::kHelperVersion << "\n"
        << "\n"
        << "Speaks the OpenShow live-source protocol over stdin/stdout.\n"
        << "\n"
        << "Usage: ndi-helper [options]\n"
        << "  --probe           print helper, protocol and NDI runtime versions\n"
        << "  --list            print the discovered NDI sources\n"
        << "  --wait-ms N       discovery wait for --list (default 1500, max 10000)\n"
        << "  --fake            use the built-in colour-bar source, not NDI\n"
        << "  --input FILE      read protocol commands from FILE in server mode\n"
        << "  --version         print the helper version\n"
        << "  --help            print this text\n"
        << "\n"
        << "Server mode is the default: one JSON command per line on stdin, one\n"
        << "JSON object per line on stdout. Diagnostics go to stderr.\n";
}

int printJson(const openshow::ndi::json::Value& value) {
    std::cout << openshow::ndi::json::write(value) << std::endl;
    return std::cout.good() ? 0 : 1;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "ndi-helper: %s\n", message.c_str());
    openshow::ndi::json::Value object = openshow::ndi::json::Value::makeObject();
    object.object.push_back({"ok", openshow::ndi::json::Value::makeBool(false)});
    object.object.push_back({"error", openshow::ndi::json::Value::makeString(message)});
    printJson(object);
    return 1;
}

int runProbe(Backend& backend) {
    openshow::ndi::ProbeInfo info;
    std::string error;
    if (!backend.probe(&info, &error)) {
        return fail(error.empty() ? "probe failed" : error);
    }
    openshow::ndi::json::Value object = openshow::ndi::json::Value::makeObject();
    object.object.push_back({"ok", openshow::ndi::json::Value::makeBool(true)});
    object.object.push_back({"helper", openshow::ndi::json::Value::makeString(info.helperVersion)});
    object.object.push_back({"protocol", openshow::ndi::json::Value::makeInteger(info.protocolVersion)});
    object.object.push_back({"ndi", openshow::ndi::json::Value::makeString(info.ndiVersion)});
    object.object.push_back({"canList", openshow::ndi::json::Value::makeBool(info.canList)});
    object.object.push_back({"canReceive", openshow::ndi::json::Value::makeBool(info.canReceive)});
    return printJson(object);
}

int runList(Backend& backend, std::uint32_t waitMs) {
    std::vector<openshow::ndi::SourceInfo> sources;
    std::string error;
    if (!backend.list(waitMs, &sources, &error)) {
        return fail(error.empty() ? "list failed" : error);
    }
    openshow::ndi::json::Value array = openshow::ndi::json::Value::makeArray();
    for (const auto& source : sources) {
        openshow::ndi::json::Value item = openshow::ndi::json::Value::makeObject();
        item.object.push_back({"name", openshow::ndi::json::Value::makeString(source.name)});
        item.object.push_back({"url", openshow::ndi::json::Value::makeString(source.url)});
        array.array.push_back(std::move(item));
    }
    openshow::ndi::json::Value object = openshow::ndi::json::Value::makeObject();
    object.object.push_back({"ok", openshow::ndi::json::Value::makeBool(true)});
    object.object.push_back({"sources", std::move(array)});
    return printJson(object);
}

}  // namespace

int main(int argc, char** argv) {
    bool fake = false;
    bool probe = false;
    bool list = false;
    std::uint32_t waitMs = 1500;
    std::string inputPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fake") {
            fake = true;
        } else if (arg == "--probe") {
            probe = true;
        } else if (arg == "--list") {
            list = true;
        } else if (arg == "--wait-ms") {
            if (i + 1 >= argc) {
                return fail("--wait-ms needs a value");
            }
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || value < 0) {
                return fail("--wait-ms needs a non-negative integer");
            }
            waitMs = static_cast<std::uint32_t>(std::min<long>(value, 10000));
        } else if (arg == "--input") {
            if (i + 1 >= argc) {
                return fail("--input needs a path");
            }
            inputPath = argv[++i];
        } else if (arg == "--version") {
            std::cout << openshow::ndi::kHelperVersion << std::endl;
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        } else {
            return fail("unknown option: " + arg);
        }
    }

    std::unique_ptr<Backend> backend;
#ifdef OPENSHOW_NDI_HELPER_WITH_NDI
    if (!fake) {
        backend = std::make_unique<openshow::ndi::NdiBackend>();
    }
#else
    // The fake-only build has no NDI backend; asking for one is an error rather
    // than a silent fallback, because a worker that requested NDI must know the
    // helper cannot speak it.
    if (!fake) {
        return fail("this helper was built without the NDI SDK "
                    "(configure with OPENSHOW_NDI_HELPER_WITH_NDI=ON)");
    }
#endif
    if (!backend) {
        backend = std::make_unique<openshow::ndi::FakeBackend>();
    }

    if (probe) {
        return runProbe(*backend);
    }
    if (list) {
        return runList(*backend, waitMs);
    }

    std::ifstream inputFile;
    std::istream* in = &std::cin;
    if (!inputPath.empty()) {
        inputFile.open(inputPath);
        if (!inputFile) {
            return fail("could not open --input file: " + inputPath);
        }
        in = &inputFile;
    }

    openshow::ndi::ProtocolServer server(*backend, *in, std::cout);
    return server.run();
}
