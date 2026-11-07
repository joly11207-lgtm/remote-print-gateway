#include <stdio.h>

#include <string>

#include "printers/catalog/DeviceCatalog.h"

int main() {
  std::string json;
  rtb::DeviceCatalog::toJson(json);
  printf("%s\n", json.c_str());
  return 0;
}
