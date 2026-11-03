#ifndef DECMPFSFILE_H
#define DECMPFSFILE_H
#include "Reader.h"
#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Reads the uncompressed size from a com.apple.decmpfs attribute, or returns false if the attribute
// does not hold a decmpfs header.
bool decmpfsUncompressedSize(const std::vector<uint8_t>& attribute, uint64_t& size);

// Opens the uncompressed contents of a file that decmpfs compressed, given its com.apple.decmpfs
// attribute and a function that opens its resource fork (or returns nullptr if it has none).
// Throws function_not_implemented_error for compression types it does not know.
std::shared_ptr<Reader> openDecmpfsFile(const std::vector<uint8_t>& attribute,
	const std::function<std::shared_ptr<Reader>()>& openResourceFork, const std::string& path);

#endif
