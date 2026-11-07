#pragma once

#include "protocols/base/StubProtocol.h"

namespace rtb {

// LATER: PeriPage. Only studied within what each upstream licence permits;
// no code or protocol table is copied here in V0.1.
class PeriPageProtocol : public StubProtocol {
 public:
  ProtocolFamily family() const override { return ProtocolFamily::PeriPage; }
  const char* name() const override { return "peripage"; }
};

}  // namespace rtb
