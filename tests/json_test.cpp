// Standalone check of the Open-Meteo JSON reader:
//   c++ -std=c++17 -Isrc tests/json_test.cpp src/online_charts_json.cpp -o json_test && ./json_test [cache-file]
// With a weather cache file, its stored forecast and marine responses are parsed too.
#include <clocale>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include "online_charts_json.h"

using online_charts::JsonValue;
using online_charts::ParseJson;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

int main(int argc, char** argv) {
  // A comma-decimal locale must not change how numbers are read.
  std::setlocale(LC_NUMERIC, "pt_PT.UTF-8");

  JsonValue v;
  Check(ParseJson(R"({"a":1.5,"b":[1,-2.25e1,3E-2],"c":"xé\"y","d":null,"e":true})", &v), "object parses");
  Check(v.Get("a") && std::fabs(v.Get("a")->number - 1.5) < 1e-12, "decimal number");
  const JsonValue* b = v.Get("b");
  Check(b && b->IsArray() && b->items.size() == 3, "array size");
  Check(b && std::fabs(b->items[1].number + 22.5) < 1e-9, "negative exponent number");
  Check(b && std::fabs(b->items[2].number - 0.03) < 1e-12, "small exponent number");
  Check(v.Get("c") && v.Get("c")->text == "x\xc3\xa9\"y", "string escapes and UTF-8");
  Check(v.Get("d") && v.Get("d")->type == JsonValue::Type::kNull, "null");
  Check(v.Get("e") && v.Get("e")->boolean, "true");
  Check(ParseJson("[{\"hourly\":{\"time\":[1,2]}},{\"hourly\":{\"time\":[3]}}]", &v) && v.items.size() == 2,
        "multi-location array");
  Check(!ParseJson("{\"a\":}", &v), "rejects a missing value");
  Check(!ParseJson("[1,2", &v), "rejects an unterminated array");
  Check(!ParseJson("{} x", &v), "rejects trailing text");

  if (argc > 1) {
    std::ifstream file(argv[1]);
    std::string line;
    int index = 0;
    while (std::getline(file, line)) {
      if (++index < 3 || line.empty()) continue;
      Check(ParseJson(line, &v), "cached Open-Meteo response parses");
      const JsonValue* first = v.IsArray() && !v.items.empty() ? &v.items[0] : &v;
      const JsonValue* hourly = first->Get("hourly");
      Check(hourly && hourly->Get("time") && !hourly->Get("time")->items.empty(), "hourly times present");
      if (hourly && hourly->Get("time")) {
        std::cout << "response " << index - 2 << ": " << hourly->Get("time")->items.size()
                  << " hours" << (v.IsArray() ? ", " + std::to_string(v.items.size()) + " locations" : "")
                  << '\n';
      }
    }
  }
  std::cout << (failures ? "FAILED" : "OK") << '\n';
  return failures ? 1 : 0;
}
