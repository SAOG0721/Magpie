#include "pch.h"
#include "HdrProtocol.h"

namespace Magpie {

const HdrFormatRoute* SelectDefaultHdrRoute(const HdrFormatRoutes& routes) noexcept {
    const HdrFormatRoute* firstHdrNative = nullptr;
    const HdrFormatRoute* firstHdrAdapter = nullptr;

    for (const HdrFormatRoute& route : routes) {
        if (!route.IsValid()) {
            continue;
        }

        if (route.defaultForHdr) {
            return &route;
        }

        if (route.IsPresentationTerminal()) {
            continue;
        }

        if (route.IsHdrNative() && !firstHdrNative) {
            firstHdrNative = &route;
        } else if (route.IsHdrAdapter() && !firstHdrAdapter) {
            firstHdrAdapter = &route;
        }
    }

    return firstHdrNative ? firstHdrNative : firstHdrAdapter;
}

const HdrFormatRoute* SelectDefaultSdrRoute(const HdrFormatRoutes& routes) noexcept {
    const HdrFormatRoute* firstAccepted = nullptr;

    for (const HdrFormatRoute& route : routes) {
        if (!route.IsValid() || route.IsPresentationTerminal()) {
            continue;
        }

        if (!firstAccepted) {
            firstAccepted = &route;
        }

        if (route.defaultForSdr) {
            return &route;
        }
    }

    return firstAccepted;
}

std::vector<const HdrFormatRoute*> GetAcceptedFormatRoutes(const HdrFormatRoutes& routes) {
    std::vector<const HdrFormatRoute*> result;
    result.reserve(routes.size());
    for (const HdrFormatRoute& route : routes) {
        if (route.IsAccepted()) {
            result.push_back(&route);
        }
    }
    return result;
}

std::vector<const HdrFormatRoute*> GetHdrNativeFormatRoutes(const HdrFormatRoutes& routes) {
    std::vector<const HdrFormatRoute*> result;
    result.reserve(routes.size());
    for (const HdrFormatRoute& route : routes) {
        if (route.IsHdrNative()) {
            result.push_back(&route);
        }
    }
    return result;
}

std::vector<const HdrFormatRoute*> GetHdrAdapterFormatRoutes(const HdrFormatRoutes& routes) {
    std::vector<const HdrFormatRoute*> result;
    result.reserve(routes.size());
    for (const HdrFormatRoute& route : routes) {
        if (route.IsHdrAdapter()) {
            result.push_back(&route);
        }
    }
    return result;
}

std::string SerializeHdrFormatRoute(const HdrFormatRoute& route) {
    return fmt::format(
        "effectId={};optionId={};inputFormat={};outputFormat={};"
        "inputTransfer={};outputTransfer={};inputRange={};outputRange={};"
        "alphaMode={};evidenceLevel={};hdrNative={};adapterProfile={};"
        "defaultForHdr={};defaultForSdr={};normalizationScale={}",
        route.effectId,
        route.optionId,
        static_cast<uint32_t>(route.inputFormat),
        static_cast<uint32_t>(route.outputFormat),
        ToString(route.inputTransfer),
        ToString(route.outputTransfer),
        ToString(route.inputRange),
        ToString(route.outputRange),
        ToString(route.alphaMode),
        ToString(route.evidenceLevel),
        route.hdrNative ? "true" : "false",
        ToString(route.adapterProfile),
        route.defaultForHdr ? "true" : "false",
        route.defaultForSdr ? "true" : "false",
        route.normalizationScale
    );
}

}
