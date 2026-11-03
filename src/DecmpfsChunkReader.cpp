#include "DecmpfsChunkReader.h"
#include <algorithm>
#include <cstring>
#include "exceptions.h"
#include "be.h"
#ifdef COMPILE_WITH_LZFSE
	#include <lzfse.h>
#endif

// decmpfs compresses data in 64 KB chunks
static const uint64_t CHUNK_SIZE = 64*1024;
// An inline chunk holds a whole file, which a damaged header can make arbitrarily large
static const uint64_t MAX_INLINE_SIZE = 16*1024*1024;
// First byte of a chunk stored uncompressed
static const uint8_t RAW_CHUNK = 0x06;

DecmpfsChunkReader::DecmpfsChunkReader(std::shared_ptr<Reader> parent, uint64_t uncompressedSize, Codec codec, bool singleChunk)
: m_reader(parent), m_uncompressedSize(uncompressedSize), m_codec(codec)
{
	if (singleChunk)
	{
		if (uncompressedSize > MAX_INLINE_SIZE || m_reader->length() > UINT32_MAX)
			throw io_error("Inline decmpfs data is too large");
		m_chunkSize = std::max<uint64_t>(uncompressedSize, 1);
		m_offsets.push_back(0);
		m_offsets.push_back(m_reader->length());
		return;
	}

	uint32_t tableSize;

	if (m_reader->read(&tableSize, sizeof(tableSize), 0) != sizeof(tableSize))
		throw io_error("Short read of decmpfs chunk table");
	tableSize = le(tableSize);

	const uint64_t chunks = (uncompressedSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
	if (tableSize % 4 || tableSize / 4 != chunks + 1 || tableSize > m_reader->length())
		throw io_error("Invalid decmpfs chunk table");

	m_chunkSize = CHUNK_SIZE;
	m_offsets.resize(chunks + 1);
	if (m_reader->read(m_offsets.data(), tableSize, 0) != int32_t(tableSize))
		throw io_error("Short read of decmpfs chunk table");

	for (size_t i = 0; i < m_offsets.size(); i++)
	{
		m_offsets[i] = le(m_offsets[i]);
		if (i > 0 && m_offsets[i] < m_offsets[i-1])
			throw io_error("decmpfs chunk offsets are out of order");
	}
	if (m_offsets.back() > m_reader->length())
		throw io_error("decmpfs chunk lies outside the resource fork");
}

void DecmpfsChunkReader::adviseOptimalBlock(uint64_t offset, uint64_t& blockStart, uint64_t& blockEnd)
{
	blockStart = offset - offset % m_chunkSize;
	blockEnd = std::min(blockStart + m_chunkSize, length());
}

void DecmpfsChunkReader::decodeChunk(size_t index)
{
	const uint32_t inputSize = m_offsets[index+1] - m_offsets[index];
	const uint64_t start = index * m_chunkSize;
	const size_t expected = std::min(m_chunkSize, m_uncompressedSize - start);

	m_lastChunk = SIZE_MAX;
	m_input.resize(inputSize);
	if (inputSize == 0 || m_reader->read(m_input.data(), inputSize, m_offsets[index]) != int32_t(inputSize))
		throw io_error("Short read of a decmpfs chunk");

	m_chunk.resize(expected);
	if (m_input[0] == RAW_CHUNK)
	{
		if (inputSize - 1 != expected)
			throw io_error("Uncompressed decmpfs chunk has the wrong size");
		memcpy(m_chunk.data(), m_input.data() + 1, expected);
		m_lastChunk = index;
		return;
	}

#ifdef COMPILE_WITH_LZFSE
	if (m_codec == Codec::LZVN)
	{
		// lzfse decodes LZVN only inside its own framing, so wrap the chunk in an LZVN block header
		// (magic "bvxn", raw size, payload size) and end the stream with "bvx$".
		const uint32_t header[3] = { le(uint32_t(0x6e787662)), le(uint32_t(expected)), le(inputSize) };
		const uint32_t trailer = le(uint32_t(0x24787662));

		m_input.insert(m_input.begin(), reinterpret_cast<const uint8_t*>(header), reinterpret_cast<const uint8_t*>(header + 3));
		m_input.insert(m_input.end(), reinterpret_cast<const uint8_t*>(&trailer), reinterpret_cast<const uint8_t*>(&trailer + 1));
	}

	// One spare byte tells a chunk that decodes to too much from one that fits exactly.
	m_chunk.resize(expected + 1);
	m_scratch.resize(lzfse_decode_scratch_size());
	if (lzfse_decode_buffer(m_chunk.data(), m_chunk.size(), m_input.data(), m_input.size(), m_scratch.data()) != expected)
		throw io_error(m_codec == Codec::LZVN ? "LZVN decompression of a decmpfs chunk failed" : "LZFSE decompression of a decmpfs chunk failed");
	m_chunk.resize(expected);
	m_lastChunk = index;
#else
	throw function_not_implemented_error("darling-dmg was built without lzfse, which LZVN and LZFSE decmpfs data need");
#endif
}

int32_t DecmpfsChunkReader::read(void* buf, int32_t count, uint64_t offset)
{
	int32_t done = 0;

	if (count <= 0 || offset >= m_uncompressedSize)
		return 0;
	if (uint64_t(count) > m_uncompressedSize - offset)
		count = m_uncompressedSize - offset;

	while (done < count)
	{
		const uint64_t position = offset + done;
		const size_t index = position / m_chunkSize;
		const uint64_t inChunk = position % m_chunkSize;

		if (index != m_lastChunk)
			decodeChunk(index);

		const int32_t thisTime = std::min<uint64_t>(count - done, m_chunk.size() - inChunk);
		memcpy(static_cast<uint8_t*>(buf) + done, m_chunk.data() + inChunk, thisTime);
		done += thisTime;
	}

	return done;
}

uint64_t DecmpfsChunkReader::length()
{
	return m_uncompressedSize;
}
