// Decodes xz runs built in memory, as DMGPartition::readRun() does, and checks that damaged
// or oversized streams are rejected rather than returning the bytes they still decode to.
#include "DMGDecompressor.h"
#include "MemoryReader.h"
#include <lzma.h>
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

static std::vector<uint8_t> payload()
{
	std::vector<uint8_t> data(1024);
	for (size_t i = 0; i < data.size(); i++)
		data[i] = uint8_t(i * 7 + i / 13);
	return data;
}

static std::vector<uint8_t> encode(const std::vector<uint8_t>& data)
{
	std::vector<uint8_t> out(lzma_stream_buffer_bound(data.size()));
	size_t size = 0;

	if (lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr, data.data(), data.size(), out.data(), &size, out.size()) != LZMA_OK)
		throw std::runtime_error("lzma_easy_buffer_encode failed");
	out.resize(size);
	return out;
}

static uint64_t readVarint(const std::vector<uint8_t>& b, size_t& pos)
{
	uint64_t value = 0;
	for (int shift = 0;; shift += 7)
	{
		uint8_t byte = b.at(pos++);
		value |= uint64_t(byte & 0x7f) << shift;
		if (!(byte & 0x80))
			return value;
	}
}

// Offset of the single block's CRC64, found through the stream footer and the index
// (xz file format 1.0.4, sections 2.1.2, 3.3 and 4).
static size_t checkOffset(const std::vector<uint8_t>& stream)
{
	uint32_t backward;
	memcpy(&backward, &stream[stream.size() - 8], sizeof(backward));
	size_t pos = stream.size() - 12 - (size_t(backward) + 1) * 4 + 1; // past the index indicator
	if (readVarint(stream, pos) != 1)
		throw std::runtime_error("expected one block");
	uint64_t unpadded = readVarint(stream, pos);
	return 12 + unpadded - 8;
}

// Decodes a run the way DMGPartition::readRun() does for a read that reaches the run's end.
static std::vector<uint8_t> decodeRun(const std::vector<uint8_t>& stream, int32_t runLength)
{
	std::shared_ptr<Reader> reader = std::make_shared<MemoryReader>(stream.data(), stream.size());
	std::unique_ptr<DMGDecompressor> decompressor(DMGDecompressor::create(RunType::LZMA, reader));
	std::vector<uint8_t> out(runLength);

	if (decompressor->decompress(out.data(), runLength, 0) < runLength)
		throw std::runtime_error("short run");
	decompressor->verifyEnd();
	return out;
}

// Returns why decoding the run failed, or an empty string if it succeeded.
static std::string rejection(const std::vector<uint8_t>& stream, int32_t runLength)
{
	try
	{
		decodeRun(stream, runLength);
		return "";
	}
	catch (const std::exception& e)
	{
		return e.what();
	}
}

static bool rejected(const std::vector<uint8_t>& stream, int32_t runLength)
{
	return !rejection(stream, runLength).empty();
}

int main()
{
	const std::vector<uint8_t> data = payload();
	const std::vector<uint8_t> stream = encode(data);
	const int32_t length = data.size();

	expect(decodeRun(stream, length) == data, "a complete stream decodes to its data");

	for (size_t cut : { 1, 12, 24 })
	{
		std::vector<uint8_t> truncated(stream.begin(), stream.end() - cut);
		expect(rejected(truncated, length), "a stream missing its last " + std::to_string(cut) + " bytes is rejected");
	}

	std::vector<uint8_t> badCheck = stream;
	badCheck[checkOffset(stream)] ^= 1;
	expect(rejected(badCheck, length), "a stream whose CRC64 does not match is rejected");

	std::vector<uint8_t> padded = stream;
	padded.insert(padded.end(), 4, 0);
	expect(rejected(padded, length), "a run with bytes after its stream is rejected");

	expect(rejected(stream, length - 512), "a stream longer than its run is rejected");

	// The block header's LZMA2 filter properties hold the dictionary size; 40 is the largest
	// (4 GiB - 1). Its header CRC32 is recomputed so that only the memory limit can object
	// (xz file format 1.0.4, sections 3.1 and 5.3.1).
	std::vector<uint8_t> greedy = stream;
	size_t header = 12, headerSize = (greedy[header] + 1) * 4, pos = header + 2;
	uint8_t flags = greedy[header + 1];
	if (flags & 0x40)
		readVarint(greedy, pos); // compressed size
	if (flags & 0x80)
		readVarint(greedy, pos); // uncompressed size
	if ((flags & 0x03) == 0 && readVarint(greedy, pos) == LZMA_FILTER_LZMA2 && readVarint(greedy, pos) == 1)
	{
		greedy[pos] = 40;
		uint32_t crc = lzma_crc32(&greedy[header], headerSize - 4, 0);
		memcpy(&greedy[header + headerSize - 4], &crc, sizeof(crc));
		expect(rejection(greedy, length).find("128 MiB") != std::string::npos,
			"a stream asking for a 4 GiB dictionary is rejected by the memory limit");
	}
	else
		expect(false, "the block header holds one LZMA2 filter");

	return failures ? 1 : 0;
}
