#include "DecmpfsFile.h"
#include "DecmpfsChunkReader.h"
#include "HFSZlibReader.h"
#include "MemoryReader.h"
#include "ResourceFork.h"
#include "decmpfs.h"
#include "exceptions.h"
#include "be.h"
#include <cstring>

static bool readHeader(const std::vector<uint8_t>& attribute, decmpfs_disk_header& hdr)
{
	if (attribute.size() < sizeof(hdr))
		return false;
	memcpy(&hdr, attribute.data(), sizeof(hdr));
	return le(hdr.compression_magic) == DECMPFS_MAGIC;
}

bool decmpfsUncompressedSize(const std::vector<uint8_t>& attribute, uint64_t& size)
{
	decmpfs_disk_header hdr;

	if (!readHeader(attribute, hdr))
		return false;
	size = le(hdr.uncompressed_size);
	return true;
}

std::shared_ptr<Reader> openDecmpfsFile(const std::vector<uint8_t>& attribute,
	const std::function<std::shared_ptr<Reader>()>& openResourceFork, const std::string& path)
{
	decmpfs_disk_header hdr;

	if (!readHeader(attribute, hdr))
		throw io_error("Invalid decmpfs attribute: " + path);

	const uint32_t type = le(hdr.compression_type);
	const uint64_t size = le(hdr.uncompressed_size);
	const uint8_t* inlineData = attribute.data() + sizeof(hdr);
	const size_t inlineSize = attribute.size() - sizeof(hdr);
	std::shared_ptr<Reader> file;

	auto resourceFork = [&]() {
		std::shared_ptr<Reader> fork = openResourceFork();
		if (!fork)
			throw io_error("Compressed file has no resource fork: " + path);
		return fork;
	};

	switch (DecmpfsCompressionType(type))
	{
		case DecmpfsCompressionType::UncompressedInline:
			if (size > inlineSize)
				throw io_error("Inline decmpfs data is shorter than the file: " + path);
			return std::make_shared<MemoryReader>(inlineData, size);
		case DecmpfsCompressionType::CompressedInline:
			file = std::make_shared<MemoryReader>(inlineData, inlineSize);
			return std::make_shared<HFSZlibReader>(file, size, true);
		case DecmpfsCompressionType::CompressedResourceFork:
		{
			ResourceFork rsrc(resourceFork());
			file = rsrc.getResource(DECMPFS_MAGIC, DECMPFS_ID);
			if (!file)
				throw function_not_implemented_error("Could not find decmpfs resource in resource fork");
			return std::make_shared<HFSZlibReader>(file, size);
		}
		case DecmpfsCompressionType::LZVNInline:
		case DecmpfsCompressionType::LZFSEInline:
		{
			const auto codec = DecmpfsCompressionType(type) == DecmpfsCompressionType::LZVNInline
				? DecmpfsChunkReader::Codec::LZVN : DecmpfsChunkReader::Codec::LZFSE;
			file = std::make_shared<MemoryReader>(inlineData, inlineSize);
			return std::make_shared<DecmpfsChunkReader>(file, size, codec, true);
		}
		case DecmpfsCompressionType::LZVNResourceFork:
		case DecmpfsCompressionType::LZFSEResourceFork:
		{
			// Unlike type 4, the chunk table starts the fork's data rather than a resource in it.
			const auto codec = DecmpfsCompressionType(type) == DecmpfsCompressionType::LZVNResourceFork
				? DecmpfsChunkReader::Codec::LZVN : DecmpfsChunkReader::Codec::LZFSE;
			return std::make_shared<DecmpfsChunkReader>(resourceFork(), size, codec);
		}
		default:
			throw function_not_implemented_error("Unknown compression type: " + std::to_string(type));
	}
}
