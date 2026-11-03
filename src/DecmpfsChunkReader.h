#ifndef DECMPFSCHUNKREADER_H
#define DECMPFSCHUNKREADER_H
#include "Reader.h"
#include <stdint.h>
#include <memory>
#include <vector>

// Reads a file that decmpfs compressed with LZVN or LZFSE, in 64 KB chunks that are each
// decompressed on their own.
//
// Stored inline, the whole file is one chunk. Stored in the resource fork, the fork starts with a
// table of little-endian 32-bit offsets, the first of which is the table's own size: chunk i runs
// from offset i to offset i+1. A chunk whose first byte is 0x06 holds the rest of its bytes
// uncompressed.
class DecmpfsChunkReader : public Reader
{
public:
	enum class Codec { LZVN, LZFSE };

	DecmpfsChunkReader(std::shared_ptr<Reader> parent, uint64_t uncompressedSize, Codec codec, bool singleChunk = false);

	virtual int32_t read(void* buf, int32_t count, uint64_t offset) override;
	virtual uint64_t length() override;
	virtual void adviseOptimalBlock(uint64_t offset, uint64_t& blockStart, uint64_t& blockEnd) override;
private:
	void decodeChunk(size_t index);
private:
	std::shared_ptr<Reader> m_reader;
	uint64_t m_uncompressedSize;
	Codec m_codec;
	uint64_t m_chunkSize;
	std::vector<uint32_t> m_offsets;
	std::vector<uint8_t> m_input, m_chunk, m_scratch;
	size_t m_lastChunk = SIZE_MAX;
};

#endif
