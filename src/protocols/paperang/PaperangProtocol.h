#pragma once

#include "protocols/base/StubProtocol.h"

namespace rtb {

// LATER: Paperang. Only studied within what each upstream licence permits;
// no code or protocol table is copied here in V0.1.
class PaperangProtocol : public StubProtocol {
 public:
  ProtocolFamily family() const override { return ProtocolFamily::Paperang; }
  const char* name() const override { return "paperang"; }
};

}  // namespace rtb
