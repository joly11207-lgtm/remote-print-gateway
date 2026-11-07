#include "app/Discovery.h"

#include <string>
#include <vector>

namespace rtb {

const char* discoveryStageName(DiscoveryStage stage) {
  switch (stage) {
    case DiscoveryStage::Ae30Filtered: return "ae30_filtered";
    case DiscoveryStage::UnfilteredFallback: return "unfiltered_fallback";
    case DiscoveryStage::NotFound: return "not_found";
    default: return "none";
  }
}

namespace {

CatalogMatch bestImplementedTinyProfile(const std::vector<Endpoint>& endpoints,
                                        Endpoint& best_endpoint) {
  CatalogMatch best;
  for (size_t i = 0; i < endpoints.size(); ++i) {
    const CatalogMatch candidate = DeviceCatalog::match(endpoints[i]);
    if (candidate.profile && candidate.profile->implemented &&
        candidate.profile->protocol_family == ProtocolFamily::TinyIPrint &&
        candidate.score > best.score) {
      best = candidate;
      best_endpoint = endpoints[i];
    }
  }
  return best;
}

}  // namespace

DiscoveryResult discoverPrinter(PrinterTransport& transport,
                                uint32_t filtered_duration_ms,
                                uint32_t fallback_duration_ms) {
  DiscoveryResult out;
  std::vector<Endpoint> candidates;

  // ---- Phase 1: prefer the advertised AE30 service ------------------------
  ScanRequest filtered;
  filtered.duration_ms = filtered_duration_ms;
  filtered.service_uuid_filter = "AE30";
  ScanResult first;
  out.scan_rounds = 1;
  if (transport.scan(filtered, first) == Result::Ok) {
    for (size_t i = 0; i < first.endpoints.size(); ++i) {
      candidates.push_back(first.endpoints[i]);
      ++out.endpoints_seen;
    }
  }
  Endpoint best_endpoint;
  out.match = bestImplementedTinyProfile(candidates, best_endpoint);
  if (out.match.profile) {
    out.endpoint = best_endpoint;
    out.stage = DiscoveryStage::Ae30Filtered;
    out.result = Result::Ok;
    return out;
  }

  // ---- Phase 2: no AE30 advertisement -> scan everything ------------------
  // DeviceCatalog then decides from name pattern, advertisement evidence and
  // protocol fingerprint. This does NOT weaken the later connect() check.
  ScanRequest unfiltered;
  unfiltered.duration_ms = fallback_duration_ms;
  unfiltered.service_uuid_filter.clear();
  ScanResult second;
  out.scan_rounds = 2;
  if (transport.scan(unfiltered, second) == Result::Ok) {
    for (size_t i = 0; i < second.endpoints.size(); ++i) {
      const Endpoint& ep = second.endpoints[i];
      ++out.endpoints_seen;
      bool duplicate = false;
      for (size_t j = 0; j < candidates.size(); ++j) {
        if (!ep.address.empty() && candidates[j].address == ep.address) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) candidates.push_back(ep);
    }
  }
  out.match = bestImplementedTinyProfile(candidates, best_endpoint);
  if (out.match.profile) {
    out.endpoint = best_endpoint;
    out.stage = DiscoveryStage::UnfilteredFallback;
    out.result = Result::Ok;
    return out;
  }

  out.stage = DiscoveryStage::NotFound;
  out.result = Result::NotFound;
  return out;
}

Endpoint prepareConnectEndpoint(const Endpoint& discovered, const DeviceProfile& profile) {
  Endpoint ep = discovered;
  // Always profile-owned, never advertisement-owned: connect() must still
  // discover and verify AE30 / AE01 / AE02 on the real GATT server.
  ep.service_uuid = profile.service_uuid;
  ep.write_uuid = profile.write_uuid;
  ep.notify_uuid = profile.notify_uuid;
  return ep;
}

}  // namespace rtb
