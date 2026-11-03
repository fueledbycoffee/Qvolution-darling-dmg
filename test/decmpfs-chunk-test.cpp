// Decodes decmpfs LZVN and LZFSE data built in memory, inline and as resource-fork chunk tables,
// and checks that damaged tables and chunks are rejected rather than read as wrong bytes.
#include "DecmpfsChunkReader.h"
#include "MemoryReader.h"
#include <lzfse.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static int failures = 0;

static void expect(bool ok, const std::string& what)
{
	std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
	if (!ok)
		failures++;
}

static std::vector<uint8_t> payload(size_t size)
{
	std::vector<uint8_t> data(size);
	for (size_t i = 0; i < data.size(); i++)
		data[i] = uint8_t((i / 3) ^ (i / 1000));
	return data;
}

static std::vector<uint8_t> encodeLZFSE(const uint8_t* data, size_t size)
{
	std::vector<uint8_t> out(size + 4096);
	size_t written = lzfse_encode_buffer(out.data(), out.size(), data, size, nullptr);

	if (written == 0)
		throw std::runtime_error("lzfse_encode_buffer failed");
	out.resize(written);
	return out;
}

// lzfse encodes inputs this small as a single LZVN block: strip its 12-byte "bvxn" header and
// the 4-byte "bvx$" end of stream to leave the bare LZVN data that decmpfs stores.
static std::vector<uint8_t> encodeLZVN(const std::vector<uint8_t>& data)
{
	std::vector<uint8_t> stream = encodeLZFSE(data.data(), data.size());

	if (stream.size() < 16 || memcmp(stream.data(), "bvxn", 4) != 0 || memcmp(&stream[stream.size() - 4], "bvx$", 4) != 0)
		throw std::runtime_error("lzfse did not produce a single LZVN block");
	return std::vector<uint8_t>(stream.begin() + 12, stream.end() - 4);
}

// A resource fork holding a chunk table followed by the chunks.
static std::vector<uint8_t> fork(const std::vector<std::vector<uint8_t>>& chunks)
{
	std::vector<uint32_t> table(1, (chunks.size() + 1) * 4);
	std::vector<uint8_t> body;

	for (const auto& chunk : chunks)
	{
		body.insert(body.end(), chunk.begin(), chunk.end());
		table.push_back(table[0] + body.size());
	}

	std::vector<uint8_t> out(reinterpret_cast<const uint8_t*>(table.data()), reinterpret_cast<const uint8_t*>(table.data() + table.size()));
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

static std::vector<uint8_t> readAll(const std::vector<uint8_t>& stored, uint64_t size, DecmpfsChunkReader::Codec codec, bool singleChunk)
{
	std::shared_ptr<Reader> reader = std::make_shared<MemoryReader>(stored.data(), stored.size());
	DecmpfsChunkReader decmpfs(reader, size, codec, singleChunk);
	std::vector<uint8_t> out(size);

	if (decmpfs.read(out.data(), out.size(), 0) != int32_t(out.size()))
		throw std::runtime_error("short read");
	return out;
}

static bool rejected(const std::vector<uint8_t>& stored, uint64_t size, DecmpfsChunkReader::Codec codec, bool singleChunk = false)
{
	try
	{
		readAll(stored, size, codec, singleChunk);
		return false;
	}
	catch (const std::exception&)
	{
		return true;
	}
}

int main()
{
	const auto LZVN = DecmpfsChunkReader::Codec::LZVN;
	const auto LZFSE = DecmpfsChunkReader::Codec::LZFSE;
	const uint32_t CHUNK = 64 * 1024;

	// LZVN
	const std::vector<uint8_t> small = payload(3000);
	const std::vector<uint8_t> lzvn = encodeLZVN(small);

	expect(readAll(lzvn, small.size(), LZVN, true) == small, "inline LZVN decodes to its data");
	expect(readAll(fork({ lzvn }), small.size(), LZVN, false) == small, "a one-chunk LZVN fork decodes to its data");
	expect(rejected(std::vector<uint8_t>(lzvn.begin(), lzvn.end() - 8), small.size(), LZVN, true), "LZVN missing its end of stream is rejected");
	expect(rejected(lzvn, small.size() + 1, LZVN, true), "LZVN that decodes to too few bytes is rejected");
	expect(rejected(lzvn, small.size() - 1, LZVN, true), "LZVN that decodes to too many bytes is rejected");

	// LZFSE in three chunks, the middle one stored uncompressed
	const std::vector<uint8_t> data = payload(2 * CHUNK + 12345);
	std::vector<uint8_t> raw(1, 0x06);
	raw.insert(raw.end(), data.begin() + CHUNK, data.begin() + 2 * CHUNK);
	const std::vector<std::vector<uint8_t>> chunks = {
		encodeLZFSE(data.data(), CHUNK), raw, encodeLZFSE(data.data() + 2 * CHUNK, data.size() - 2 * CHUNK) };
	const std::vector<uint8_t> stored = fork(chunks);

	expect(readAll(stored, data.size(), LZFSE, false) == data, "an LZFSE fork with an uncompressed chunk decodes to its data");
	{
		std::shared_ptr<Reader> reader = std::make_shared<MemoryReader>(stored.data(), stored.size());
		DecmpfsChunkReader decmpfs(reader, data.size(), LZFSE);
		std::vector<uint8_t> part(CHUNK + 100);
		bool ok = decmpfs.read(part.data(), part.size(), CHUNK - 50) == int32_t(part.size())
			&& memcmp(part.data(), &data[CHUNK - 50], part.size()) == 0;
		ok = ok && decmpfs.read(part.data(), part.size(), data.size() - 10) == 10 && memcmp(part.data(), &data[data.size() - 10], 10) == 0;
		expect(ok, "reads across chunk boundaries and past the end return the right bytes");
	}

	expect(rejected(fork({ chunks[0], chunks[1] }), data.size(), LZFSE), "a table with too few chunks is rejected");
	expect(rejected(stored, 2 * CHUNK, LZFSE), "a table with too many chunks is rejected");
	{
		std::vector<uint8_t> swapped = stored;
		memcpy(&swapped[4], &stored[8], 4);
		memcpy(&swapped[8], &stored[4], 4);
		expect(rejected(swapped, data.size(), LZFSE), "chunk offsets out of order are rejected");
	}
	{
		std::vector<uint8_t> beyond = stored;
		beyond.resize(stored.size() - 1);
		expect(rejected(beyond, data.size(), LZFSE), "a chunk past the end of the fork is rejected");
	}
	{
		std::vector<uint8_t> shortRaw = raw;
		shortRaw.pop_back();
		expect(rejected(fork({ chunks[0], shortRaw, chunks[2] }), data.size(), LZFSE), "an uncompressed chunk of the wrong size is rejected");
	}
	{
		std::vector<uint8_t> damaged = chunks[2];
		damaged.resize(damaged.size() - 4);
		expect(rejected(fork({ chunks[0], chunks[1], damaged }), data.size(), LZFSE), "a truncated LZFSE chunk is rejected");
	}

	return failures ? 1 : 0;
}
