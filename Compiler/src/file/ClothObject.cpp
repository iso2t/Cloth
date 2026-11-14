#include <file/ClothObject.hpp>

/**
 * The two supported file types are dictated here.
 * All other file types are rejected.
 */
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
