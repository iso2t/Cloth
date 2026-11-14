#include "../../include/file/ClothObject.hpp"

std::string_view to_string(Extension type) {
  switch (type) {
  case CLOTH_OBJECT:
    return "co";
  case LIBRARY:
    return "clib";
  default:
    return "UNKNOWN";
  }
}