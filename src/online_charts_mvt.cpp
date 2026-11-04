#include "online_charts_mvt.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace online_charts {
namespace {

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool End() const { return pos_ == size_; }
  size_t Remaining() const { return size_ - pos_; }

  bool Varint(uint64_t* value) {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
      if (pos_ >= size_) return false;
      const uint8_t byte = data_[pos_++];
      if (shift == 63 && (byte & 0xfe)) return false;
      result |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if (!(byte & 0x80)) {
        *value = result;
        return true;
      }
    }
    return false;
  }

  bool Key(uint32_t* field, uint32_t* wire) {
    uint64_t key = 0;
    if (!Varint(&key) || (key >> 3) == 0 || (key >> 3) > 0x1fffffff) return false;
    *field = static_cast<uint32_t>(key >> 3);
    *wire = static_cast<uint32_t>(key & 7);
    return true;
  }

  bool Bytes(const uint8_t** data, size_t* size) {
    uint64_t length = 0;
    if (!Varint(&length) || length > Remaining()) return false;
    *data = data_ + pos_;
    *size = static_cast<size_t>(length);
    pos_ += static_cast<size_t>(length);
    return true;
  }

  bool String(std::string* value) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!Bytes(&data, &size)) return false;
    value->assign(reinterpret_cast<const char*>(data), size);
    return true;
  }

  bool Fixed32(uint32_t* value) {
    if (Remaining() < 4) return false;
    std::memcpy(value, data_ + pos_, 4);
    pos_ += 4;
    return true;
  }

  bool Fixed64(uint64_t* value) {
    if (Remaining() < 8) return false;
    std::memcpy(value, data_ + pos_, 8);
    pos_ += 8;
    return true;
  }

  bool Skip(uint32_t wire) {
    uint64_t value = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    switch (wire) {
      case 0:
        return Varint(&value);
      case 1:
        return Fixed64(&value);
      case 2:
        return Bytes(&data, &size);
      case 5:
        {
          uint32_t value32 = 0;
          return Fixed32(&value32);
        }
      default:
        return false;
    }
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

struct Value {
  enum class Type { kEmpty, kNumber, kString, kBool };
  Type type = Type::kEmpty;
  double number = 0.0;
  std::string text;
  bool boolean = false;
};

bool ParseValue(const uint8_t* data, size_t size, Value* value) {
  Reader reader(data, size);
  while (!reader.End()) {
    uint32_t field = 0, wire = 0;
    if (!reader.Key(&field, &wire)) return false;
    uint64_t integer = 0;
    if (field == 1 && wire == 2) {
      value->type = Value::Type::kString;
      if (!reader.String(&value->text)) return false;
    } else if (field == 2 && wire == 5) {
      uint32_t bits = 0;
      float number = 0.0f;
      if (!reader.Fixed32(&bits)) return false;
      std::memcpy(&number, &bits, sizeof(number));
      value->type = Value::Type::kNumber;
      value->number = number;
    } else if (field == 3 && wire == 1) {
      uint64_t bits = 0;
      double number = 0.0;
      if (!reader.Fixed64(&bits)) return false;
      std::memcpy(&number, &bits, sizeof(number));
      value->type = Value::Type::kNumber;
      value->number = number;
    } else if (field >= 4 && field <= 6 && wire == 0) {
      if (!reader.Varint(&integer)) return false;
      if (field == 6) integer = static_cast<uint64_t>((integer >> 1) ^ (~(integer & 1) + 1));
      value->type = Value::Type::kNumber;
      // int (4) and zigzag sint (6) are signed; only uint (5) is unsigned.
      // Drying heights (negative DRVAL1) are sint values.
      value->number = field == 5 ? static_cast<double>(integer)
                                 : static_cast<double>(static_cast<int64_t>(integer));
    } else if (field == 7 && wire == 0) {
      if (!reader.Varint(&integer)) return false;
      value->type = Value::Type::kBool;
      value->boolean = integer != 0;
    } else if (!reader.Skip(wire)) {
      return false;
    }
  }
  return true;
}

bool ReadUInt32Vector(const uint8_t* data, size_t size, std::vector<uint32_t>* values) {
  Reader reader(data, size);
  while (!reader.End()) {
    uint64_t value = 0;
    if (!reader.Varint(&value) || value > std::numeric_limits<uint32_t>::max()) return false;
    values->push_back(static_cast<uint32_t>(value));
  }
  return true;
}

int64_t ZigZag(uint64_t value) {
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

bool ParseGeometry(const uint8_t* data, size_t size, int geometry_type, int extent,
                   std::vector<std::vector<MvtPoint>>* parts) {
  if (extent <= 0) return false;
  Reader reader(data, size);
  std::vector<uint64_t> integers;
  while (!reader.End()) {
    uint64_t value = 0;
    if (!reader.Varint(&value)) return false;
    integers.push_back(value);
    if (integers.size() > 4000000) return false;
  }

  size_t pos = 0;
  int64_t x = 0, y = 0;
  std::vector<MvtPoint> current;
  auto flush = [&]() {
    if (!current.empty()) {
      parts->push_back(current);
      current.clear();
    }
  };
  auto append_point = [&]() {
    MvtPoint point;
    point.x = static_cast<double>(x) / extent;
    point.y = static_cast<double>(y) / extent;
    current.push_back(point);
  };

  while (pos < integers.size()) {
    const uint64_t command = integers[pos++];
    const uint32_t id = static_cast<uint32_t>(command & 7);
    const uint64_t count = command >> 3;
    if (count == 0 || count > 1000000) return false;

    if (id == 1) {  // MoveTo
      if (count > (integers.size() - pos) / 2) return false;
      if (geometry_type == 1) {
        flush();
        for (uint64_t i = 0; i < count; ++i) {
          x += ZigZag(integers[pos++]);
          y += ZigZag(integers[pos++]);
          append_point();
          flush();
        }
      } else {
        if (count != 1) return false;
        flush();
        x += ZigZag(integers[pos++]);
        y += ZigZag(integers[pos++]);
        append_point();
      }
    } else if (id == 2) {  // LineTo
      if ((geometry_type != 2 && geometry_type != 3) || current.empty() ||
          count > (integers.size() - pos) / 2) {
        return false;
      }
      for (uint64_t i = 0; i < count; ++i) {
        x += ZigZag(integers[pos++]);
        y += ZigZag(integers[pos++]);
        append_point();
      }
    } else if (id == 7) {  // ClosePath
      if (geometry_type != 3 || count != 1 || current.empty()) return false;
      // DrawPath/DrawPolygon closes rings; retaining a duplicate is unnecessary.
    } else {
      return false;
    }
  }
  flush();
  return true;
}

const Value* Property(const std::vector<uint32_t>& tags,
                      const std::vector<std::string>& keys,
                      const std::vector<Value>& values, const char* name) {
  for (size_t i = 0; i + 1 < tags.size(); i += 2) {
    const uint32_t key = tags[i];
    const uint32_t value = tags[i + 1];
    if (key < keys.size() && value < values.size() && keys[key] == name) {
      return &values[value];
    }
  }
  return nullptr;
}

double Number(const Value* value, double fallback = 0.0) {
  return value && value->type == Value::Type::kNumber &&
                 std::isfinite(value->number)
             ? value->number
             : fallback;
}

bool HasUnitSystem(const std::vector<uint32_t>& tags,
                   const std::vector<std::string>& keys,
                   const std::vector<Value>& values) {
  return Property(tags, keys, values, "sys") != nullptr;
}

bool ReadDepth(const std::vector<uint32_t>& tags,
               const std::vector<std::string>& keys,
               const std::vector<Value>& values, const char* name,
               double* depth) {
  const Value* value = Property(tags, keys, values, name);
  if (!value || value->type != Value::Type::kNumber ||
      !std::isfinite(value->number)) {
    return false;
  }
  *depth = value->number;
  return true;
}

bool ParseFeature(const uint8_t* data, size_t size, const std::string& layer,
                  int extent, const std::vector<std::string>& keys,
                  const std::vector<Value>& values, MvtTile* tile) {
  Reader reader(data, size);
  std::vector<uint32_t> tags;
  int geometry_type = 0;
  const uint8_t* geometry = nullptr;
  size_t geometry_size = 0;

  while (!reader.End()) {
    uint32_t field = 0, wire = 0;
    if (!reader.Key(&field, &wire)) return false;
    if (field == 2 && wire == 2) {
      const uint8_t* bytes = nullptr;
      size_t length = 0;
      if (!reader.Bytes(&bytes, &length) || !ReadUInt32Vector(bytes, length, &tags)) {
        return false;
      }
    } else if (field == 3 && wire == 0) {
      uint64_t type = 0;
      if (!reader.Varint(&type) || type > 3) return false;
      geometry_type = static_cast<int>(type);
    } else if (field == 4 && wire == 2) {
      if (!reader.Bytes(&geometry, &geometry_size)) return false;
    } else if (!reader.Skip(wire)) {
      return false;
    }
  }
  if (!geometry) return true;

  // Open Waters publishes alternative unit-specific features in the same tile.
  // Keep both contour/sounding sets so the display units can be changed without
  // downloading again. Depth areas carrying a unit system are alternatives too;
  // the browser reference displays only the base, untagged depare features.
  if (layer == "depare" && HasUnitSystem(tags, keys, values)) return true;

  std::vector<std::vector<MvtPoint>> parts;
  if (!ParseGeometry(geometry, geometry_size, geometry_type, extent, &parts)) return false;

  if (layer == "depare" && geometry_type == 3) {
    MvtArea area;
    area.rings.swap(parts);
    const Value* depth = Property(tags, keys, values, "drval1");
    area.has_drval1 = depth && depth->type == Value::Type::kNumber;
    area.drval1 = Number(depth);
    area.rank = Number(Property(tags, keys, values, "rank"));
    if (!area.rings.empty()) tile->areas.push_back(std::move(area));
  } else if (layer == "contours" && geometry_type == 2) {
    MvtContour contour;
    contour.lines.swap(parts);
    contour.has_depth_m = ReadDepth(tags, keys, values, "depth_abs_m", &contour.depth_m);
    if (!contour.has_depth_m) {
      contour.has_depth_m = ReadDepth(tags, keys, values, "depth_m", &contour.depth_m);
    }
    contour.has_depth_ft = ReadDepth(tags, keys, values, "depth_ft", &contour.depth_ft);
    const Value* system = Property(tags, keys, values, "sys");
    contour.feet_system = system && system->type == Value::Type::kString && system->text == "ft";
    if (!contour.lines.empty() && (contour.has_depth_m || contour.has_depth_ft)) {
      tile->contours.push_back(std::move(contour));
    }
  } else if (layer == "soundings" && geometry_type == 1) {
    for (const auto& part : parts) {
      for (const auto& point : part) {
        MvtSounding sounding;
        sounding.point = point;
        sounding.has_depth_m = ReadDepth(tags, keys, values, "depth_m", &sounding.depth_m);
        sounding.has_depth_ft = ReadDepth(tags, keys, values, "depth_ft", &sounding.depth_ft);
        if (sounding.has_depth_m || sounding.has_depth_ft) {
          tile->soundings.push_back(std::move(sounding));
        }
      }
    }
  }
  return true;
}

bool ParseLayer(const uint8_t* data, size_t size, MvtTile* tile) {
  Reader reader(data, size);
  std::string name;
  std::vector<std::string> keys;
  std::vector<Value> values;
  std::vector<std::string> features;
  int extent = 4096;

  while (!reader.End()) {
    uint32_t field = 0, wire = 0;
    if (!reader.Key(&field, &wire)) return false;
    if (field == 1 && wire == 2) {
      if (!reader.String(&name)) return false;
    } else if (field == 2 && wire == 2) {
      std::string feature;
      if (!reader.String(&feature)) return false;
      features.push_back(std::move(feature));
    } else if (field == 3 && wire == 2) {
      std::string key;
      if (!reader.String(&key)) return false;
      keys.push_back(std::move(key));
    } else if (field == 4 && wire == 2) {
      const uint8_t* bytes = nullptr;
      size_t length = 0;
      Value value;
      if (!reader.Bytes(&bytes, &length) || !ParseValue(bytes, length, &value)) return false;
      values.push_back(std::move(value));
    } else if (field == 5 && wire == 0) {
      uint64_t value = 0;
      if (!reader.Varint(&value) || value == 0 || value > 0x7fffffff) return false;
      extent = static_cast<int>(value);
    } else if (!reader.Skip(wire)) {
      return false;
    }
  }

  if (name != "depare" && name != "contours" && name != "soundings") return true;
  for (const auto& feature : features) {
    if (!ParseFeature(reinterpret_cast<const uint8_t*>(feature.data()), feature.size(),
                      name, extent, keys, values, tile)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ParseMvtTile(const uint8_t* data, size_t size, MvtTile* tile) {
  if (!tile || (!data && size != 0)) return false;
  MvtTile parsed;
  Reader reader(data, size);
  while (!reader.End()) {
    uint32_t field = 0, wire = 0;
    if (!reader.Key(&field, &wire)) return false;
    if (field == 3 && wire == 2) {
      const uint8_t* bytes = nullptr;
      size_t length = 0;
      if (!reader.Bytes(&bytes, &length) || !ParseLayer(bytes, length, &parsed)) return false;
    } else if (!reader.Skip(wire)) {
      return false;
    }
  }
  *tile = std::move(parsed);
  return true;
}

}  // namespace online_charts
