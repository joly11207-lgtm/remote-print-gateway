#pragma once

#include "protocols/base/PrinterProtocol.h"

namespace rtb {

// Shared reservation skeleton for protocol families scheduled for LATER.
// It is deliberately explicit: nothing here pretends to be implemented.
class StubProtocol : public PrinterProtocol {
 public:
  ProtocolCapabilities capabilities() const override {
    ProtocolCapabilities c;
    c.implemented = false;
    c.note = "LATER: protocol family reserved in V0.1, no wire format implemented";
    return c;
  }

  Result init(const ProtocolContext&, PrinterTransport&) override {
    return Result::NotImplemented;
  }
  Result printRow(const ProtocolContext&, const uint8_t*, size_t,
                  PrinterTransport&) override {
    return Result::NotImplemented;
  }
  Result finish(const ProtocolContext&, PrinterTransport&) override {
    return Result::NotImplemented;
  }
  Result feed(const ProtocolContext&, uint16_t, PrinterTransport&) override {
    return Result::NotImplemented;
  }
  Result statusRequest(PrinterTransport&) override { return Result::NotImplemented; }
  Result parseStatus(const uint8_t*, size_t, PrinterStatus&) const override {
    return Result::NotImplemented;
  }
};

}  // namespace rtb
