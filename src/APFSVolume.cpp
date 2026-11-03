#include "APFSVolume.h"
#include "APFSBTreeNode.h"
#include "DecmpfsFile.h"
#include "MemoryReader.h"
#include "decmpfs.h"
#include "exceptions.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>

#ifndef UF_COMPRESSED
#define UF_COMPRESSED 0x00000020 // xnu bsd/sys/stat.h
#endif

// APFS has no resource forks of its own; a compressed file's is assumed to be the extended
// attribute that HFSHighLevelVolume presents an HFS+ resource fork as.
static const char RESOURCE_FORK_EA_NAME[] = "com.apple.ResourceFork";

namespace
{

// Reads a data stream from its file extents. Ranges that no extent covers are holes and read as zeros.
class APFSFileReader : public Reader
{
public:
	struct Extent
	{
		uint64_t length;
		paddr_t block;
	};

	APFSFileReader(std::shared_ptr<Reader> disk, uint32_t blockSize, std::map<uint64_t, Extent> extents, uint64_t size)
		: m_disk(disk), m_blockSize(blockSize), m_extents(std::move(extents)), m_size(size)
	{
	}

	virtual int32_t read(void* buf, int32_t count, uint64_t offset) override
	{
		int32_t done = 0;

		if (count < 0 || offset >= m_size)
			return 0;
		count = int32_t(std::min<uint64_t>(count, m_size - offset));

		while (done < count)
		{
			uint64_t pos = offset + done;
			auto next = m_extents.upper_bound(pos);
			int32_t thistime;

			if (next != m_extents.begin() && pos < std::prev(next)->first + std::prev(next)->second.length)
			{
				auto extent = std::prev(next);
				uint64_t inExtent = pos - extent->first;

				thistime = int32_t(std::min<uint64_t>(count - done, extent->second.length - inExtent));
				if (m_disk->read((char*)buf + done, thistime, uint64_t(extent->second.block) * m_blockSize + inExtent) != thistime)
					throw io_error("Short read from APFS file extent");
			}
			else
			{
				uint64_t holeEnd = next == m_extents.end() ? m_size : next->first;

				thistime = int32_t(std::min<uint64_t>(count - done, holeEnd - pos));
				memset((char*)buf + done, 0, thistime);
			}
			done += thistime;
		}
		return done;
	}

	virtual uint64_t length() override
	{
		return m_size;
	}
private:
	std::shared_ptr<Reader> m_disk;
	uint32_t m_blockSize;
	std::map<uint64_t, Extent> m_extents;
	uint64_t m_size;
};

uint64_t keyOrder(const uint8_t* key, uint16_t length)
{
	j_key_t hdr;

	if (length < sizeof(hdr))
		throw io_error("APFS file-system key is too short");
	memcpy(&hdr, key, sizeof(hdr));

	// Records sort by object identifier, then by record type.
	uint64_t idAndType = le(hdr.obj_id_and_type);
	return ((idAndType & OBJ_ID_MASK) << 4) | (idAndType >> OBJ_TYPE_SHIFT);
}

// Returns the size of an inode's data stream, which is 0 when the inode has none.
uint64_t dstreamSize(const std::vector<uint8_t>& inode)
{
	const size_t blobStart = sizeof(j_inode_val_t);
	xf_blob_t blob;

	// An inode without extended fields ends with j_inode_val_t; any tail must be a whole blob.
	if (inode.size() == blobStart)
		return 0;
	if (inode.size() < blobStart + sizeof(blob))
		throw io_error("APFS inode extended fields are truncated");
	memcpy(&blob, &inode[blobStart], sizeof(blob));

	// The x_field_t array is followed by xf_used_data bytes of values, each padded to a
	// multiple of 8 bytes.
	size_t fieldStart = blobStart + sizeof(blob);
	size_t valueStart = fieldStart + le(blob.xf_num_exts) * sizeof(x_field_t);
	size_t valueEnd = valueStart + le(blob.xf_used_data);

	if (valueEnd > inode.size())
		throw io_error("APFS inode extended fields are out of bounds");

	for (uint16_t i = 0; i < le(blob.xf_num_exts); i++)
	{
		x_field_t field;

		memcpy(&field, &inode[fieldStart + i * sizeof(field)], sizeof(field));
		if (le(field.x_size) > valueEnd - valueStart)
			throw io_error("APFS inode extended fields are out of bounds");

		if (field.x_type == INO_EXT_TYPE_DSTREAM)
		{
			j_dstream_t dstream;

			if (le(field.x_size) < sizeof(dstream))
				throw io_error("APFS inode data stream field is too short");
			memcpy(&dstream, &inode[valueStart], sizeof(dstream));
			return le(dstream.size);
		}
		valueStart += std::min<size_t>((le(field.x_size) + 7) & ~size_t(7), valueEnd - valueStart);
	}
	return 0;
}

} // namespace

APFSVolume::APFSVolume(std::shared_ptr<APFSContainer> container, oid_t volumeOid)
	: m_container(container)
{
	std::vector<uint8_t> block;
	const apfs_superblock_t* sb;
	uint64_t incompat;
	const uint64_t supportedIncompat = APFS_INCOMPAT_CASE_INSENSITIVE | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE;

	if (m_container->readObject(m_container->resolve(m_container->objectMap(), volumeOid), block) != OBJECT_TYPE_FS)
		throw io_error("APFS volume superblock expected");
	sb = reinterpret_cast<const apfs_superblock_t*>(block.data());
	if (le(sb->apfs_magic) != APFS_MAGIC)
		throw io_error("Invalid APFS volume superblock magic");

	if (!(le(sb->apfs_fs_flags) & APFS_FS_UNENCRYPTED))
		throw function_not_implemented_error("Encrypted APFS volumes are not supported");
	incompat = le(sb->apfs_incompatible_features);
	if (incompat & ~supportedIncompat)
		throw function_not_implemented_error("Unsupported APFS volume features: " + std::to_string(incompat));

	m_omap = le(sb->apfs_omap_oid);
	m_rootTree = le(sb->apfs_root_tree_oid);
	m_virtualTree = !(le(sb->apfs_root_tree_type) & OBJ_PHYSICAL);
}

void APFSVolume::forEachRecord(oid_t oid, uint8_t type, const RecordVisitor& visit)
{
	if (oid & ~OBJ_ID_MASK)
		throw io_error("Invalid APFS object identifier " + std::to_string(oid));

	const uint64_t target = (oid << 4) | type;
	// Nodes still to read, with their expected level, the next one last, so that records
	// are visited in key order without recursion.
	std::vector<std::pair<oid_t, int>> pending { { m_rootTree, -1 } };
	// A lookup reads only nodes whose key range covers its records, each once, and every
	// node is a block of its own: reading more nodes than the container has blocks means
	// the tree loops or shares nodes.
	uint64_t budget = m_container->blockCount();

	while (!pending.empty())
	{
		oid_t node = pending.back().first;
		int expectedLevel = pending.back().second;
		std::vector<uint8_t> block;
		std::vector<oid_t> children;

		pending.pop_back();
		if (budget-- == 0)
			throw io_error("APFS file-system tree lookup reads more nodes than the container has blocks");

		paddr_t paddr = m_virtualTree ? m_container->resolve(m_omap, node) : paddr_t(node);
		uint32_t type = m_container->readObject(paddr, block);
		if (type != OBJECT_TYPE_BTREE && type != OBJECT_TYPE_BTREE_NODE)
			throw io_error("APFS file-system tree node expected at block " + std::to_string(paddr));

		APFSBTreeNode btn(std::move(block), 0, 0);
		if (expectedLevel >= 0 && btn.level() != expectedLevel)
			throw io_error("APFS file-system tree node has an unexpected level");

		for (uint32_t i = 0; i < btn.count(); i++)
		{
			uint16_t keyLength, valueLength;
			const uint8_t* key = btn.key(i, &keyLength);
			uint64_t order = keyOrder(key, keyLength);

			if (order > target)
				break;

			if (btn.isLeaf())
			{
				if (order == target)
				{
					const uint8_t* value = btn.value(i, &valueLength);
					visit(key, keyLength, value, valueLength);
				}
			}
			else
			{
				// Child i holds the keys from entry i up to entry i+1.
				if (i + 1 < btn.count())
				{
					const uint8_t* nextKey = btn.key(i + 1, &keyLength);
					if (keyOrder(nextKey, keyLength) < target)
						continue;
				}
				children.push_back(btn.child(i));
			}
		}

		for (auto it = children.rbegin(); it != children.rend(); ++it)
			pending.emplace_back(*it, btn.level() - 1);
	}
}

std::map<std::string, oid_t> APFSVolume::directoryEntries(oid_t directory)
{
	std::map<std::string, oid_t> entries;

	forEachRecord(directory, APFS_TYPE_DIR_REC, [&](const uint8_t* key, uint16_t keyLength, const uint8_t* value, uint16_t valueLength)
	{
		const uint8_t* name;
		uint32_t nameLength;
		j_drec_val_t val;

		// Directory records carry either a j_drec_hashed_key_t or a j_drec_key_t; the stored
		// name length tells them apart, because it includes the final NUL.
		if (keyLength >= sizeof(j_drec_hashed_key_t)
			&& sizeof(j_drec_hashed_key_t) + (le(reinterpret_cast<const j_drec_hashed_key_t*>(key)->name_len_and_hash) & J_DREC_LEN_MASK) == keyLength)
		{
			name = key + sizeof(j_drec_hashed_key_t);
			nameLength = keyLength - sizeof(j_drec_hashed_key_t);
		}
		else if (keyLength >= sizeof(j_drec_key_t)
			&& sizeof(j_drec_key_t) + le(reinterpret_cast<const j_drec_key_t*>(key)->name_len) == keyLength)
		{
			name = key + sizeof(j_drec_key_t);
			nameLength = keyLength - sizeof(j_drec_key_t);
		}
		else
			throw io_error("Malformed APFS directory record key");

		// The name must be non-empty and end at its only NUL, so it cannot change meaning as a path.
		if (nameLength < 2 || name[nameLength - 1] != 0 || memchr(name, 0, nameLength - 1) || valueLength < sizeof(val))
			throw io_error("Malformed APFS directory record");

		memcpy(&val, value, sizeof(val));
		entries[std::string(reinterpret_cast<const char*>(name), nameLength - 1)] = le(val.file_id);
	});

	return entries;
}

std::vector<uint8_t> APFSVolume::inode(oid_t oid)
{
	std::vector<uint8_t> rv;
	bool found = false;

	forEachRecord(oid, APFS_TYPE_INODE, [&](const uint8_t*, uint16_t, const uint8_t* value, uint16_t valueLength)
	{
		if (valueLength < sizeof(j_inode_val_t))
			throw io_error("APFS inode record is too short");
		rv.assign(value, value + valueLength);
		found = true;
	});

	if (!found)
		throw io_error("APFS inode " + std::to_string(oid) + " is missing");
	return rv;
}

oid_t APFSVolume::lookup(const std::string& path)
{
	oid_t oid = ROOT_DIR_INO_NUM;
	size_t pos = 0;

	while (pos < path.size())
	{
		size_t end = path.find('/', pos);
		if (end == std::string::npos)
			end = path.size();

		if (end > pos)
		{
			std::map<std::string, oid_t> entries = directoryEntries(oid);
			auto it = entries.find(path.substr(pos, end - pos));

			if (it == entries.end())
				throw file_not_found_error(path);
			oid = it->second;
		}
		pos = end + 1;
	}
	return oid;
}

std::shared_ptr<Reader> APFSVolume::xattrReader(oid_t oid, const std::string& name)
{
	std::shared_ptr<Reader> reader;

	forEachRecord(oid, APFS_TYPE_XATTR, [&](const uint8_t* key, uint16_t keyLength, const uint8_t* value, uint16_t valueLength)
	{
		const j_xattr_key_t* k = reinterpret_cast<const j_xattr_key_t*>(key);
		const j_xattr_val_t* v = reinterpret_cast<const j_xattr_val_t*>(value);

		if (keyLength < sizeof(*k) || sizeof(*k) + le(k->name_len) != keyLength || valueLength < sizeof(*v))
			throw io_error("Malformed APFS extended attribute record");
		if (le(k->name_len) != name.size() + 1 || memcmp(k->name, name.c_str(), name.size() + 1) != 0)
			return;

		if (le(v->flags) & XATTR_DATA_EMBEDDED)
		{
			if (sizeof(*v) + le(v->xdata_len) > valueLength)
				throw io_error("APFS extended attribute data is out of bounds");
			reader = std::make_shared<MemoryReader>(v->xdata, le(v->xdata_len));
		}
		else if (le(v->flags) & XATTR_DATA_STREAM)
		{
			j_xattr_dstream_t dstream;

			if (sizeof(*v) + sizeof(dstream) > valueLength)
				throw io_error("APFS extended attribute stream is out of bounds");
			memcpy(&dstream, v->xdata, sizeof(dstream));
			reader = openStream(le(dstream.xattr_obj_id), le(dstream.dstream.size));
		}
		else
			throw io_error("APFS extended attribute has no data");
	});

	return reader;
}

bool APFSVolume::xattr(oid_t oid, const std::string& name, std::vector<uint8_t>& data)
{
	std::shared_ptr<Reader> reader = xattrReader(oid, name);

	if (!reader)
		return false;
	if (reader->length() > INT32_MAX)
		throw io_error("APFS extended attribute stream is too large");
	data.resize(reader->length());
	if (!data.empty() && reader->read(data.data(), data.size(), 0) != int32_t(data.size()))
		throw io_error("Short read of APFS extended attribute stream");
	return true;
}

std::string APFSVolume::symlinkTarget(oid_t oid)
{
	std::vector<uint8_t> data;

	if (!xattr(oid, SYMLINK_EA_NAME, data) || data.empty() || data.back() != 0)
		throw io_error("APFS symbolic link " + std::to_string(oid) + " has no target");
	if (memchr(data.data(), 0, data.size() - 1))
		throw io_error("APFS symbolic link " + std::to_string(oid) + " has a NUL in its target");
	return std::string(data.begin(), data.end() - 1);
}

std::shared_ptr<Reader> APFSVolume::openStream(oid_t streamId, uint64_t size)
{
	std::map<uint64_t, APFSFileReader::Extent> extents;

	forEachRecord(streamId, APFS_TYPE_FILE_EXTENT, [&](const uint8_t* key, uint16_t keyLength, const uint8_t* value, uint16_t valueLength)
	{
		j_file_extent_key_t k;
		j_file_extent_val_t v;

		if (keyLength != sizeof(k) || valueLength != sizeof(v))
			throw io_error("Malformed APFS file extent record");
		memcpy(&k, key, sizeof(k));
		memcpy(&v, value, sizeof(v));

		uint64_t logical = le(k.logical_addr);
		uint64_t length = le(v.len_and_flags) & J_FILE_EXTENT_LEN_MASK;
		uint64_t block = le(v.phys_block_num);
		uint64_t blockSize = m_container->blockSize();

		// The reference defines no extent flags, and requires lengths in whole blocks.
		if (le(v.len_and_flags) >> J_FILE_EXTENT_FLAG_SHIFT)
			throw io_error("APFS file extent has unknown flags");
		if (length == 0 || length % blockSize)
			throw io_error("APFS file extent length is not a whole number of blocks");
		// Block zero holds the container superblock, so no extent can start there.
		if (block == 0)
			throw io_error("APFS file extent points at block zero");
		if (block >= m_container->blockCount() || length / blockSize > m_container->blockCount() - block)
			throw io_error("APFS file extent lies outside the container");
		if (logical > UINT64_MAX - length)
			throw io_error("APFS file extent ends beyond the largest file offset");
		if (!extents.emplace(logical, APFSFileReader::Extent{ length, paddr_t(block) }).second)
			throw io_error("APFS file has two extents at one offset");
	});

	uint64_t end = 0;
	for (const auto& extent : extents)
	{
		if (extent.first < end)
			throw io_error("APFS file extents overlap");
		end = extent.first + extent.second.length;
	}

	return std::make_shared<APFSFileReader>(m_container->reader(), m_container->blockSize(), std::move(extents), size);
}

void APFSVolume::inodeToStat(oid_t oid, const std::vector<uint8_t>& inode, struct stat* st)
{
	j_inode_val_t val;

	memcpy(&val, inode.data(), sizeof(val));
	memset(st, 0, sizeof(*st));

	st->st_ino = oid;
	st->st_mode = le(val.mode);
	st->st_uid = le(val.owner);
	st->st_gid = le(val.group);
	st->st_blksize = m_container->blockSize();
	// Timestamps are nanoseconds since 1970.
	st->st_atime = le(val.access_time) / 1000000000;
	st->st_mtime = le(val.mod_time) / 1000000000;
	st->st_ctime = le(val.change_time) / 1000000000;

	if (S_ISDIR(st->st_mode))
		return;

	st->st_nlink = le(val.nchildren_or_nlink);
	if (S_ISLNK(st->st_mode))
		st->st_size = symlinkTarget(oid).size();
	else if (le(val.bsd_flags) & UF_COMPRESSED)
	{
		std::vector<uint8_t> attribute;
		uint64_t size;

		if (!xattr(oid, DECMPFS_XATTR_NAME, attribute) || !decmpfsUncompressedSize(attribute, size) || size > INT64_MAX)
			throw io_error("APFS compressed file " + std::to_string(oid) + " has no valid decmpfs attribute");
		st->st_size = size;
	}
	else
		st->st_size = dstreamSize(inode);
}

std::map<std::string, struct stat> APFSVolume::listDirectory(const std::string& path)
{
	std::map<std::string, struct stat> rv;
	oid_t directory = lookup(path);
	std::vector<uint8_t> dirInode = inode(directory);

	if (!S_ISDIR(le(reinterpret_cast<const j_inode_val_t*>(dirInode.data())->mode)))
		throw file_not_found_error(path);

	for (const auto& entry : directoryEntries(directory))
		inodeToStat(entry.second, inode(entry.second), &rv[entry.first]);

	return rv;
}

struct stat APFSVolume::stat(const std::string& path)
{
	struct stat st;
	oid_t oid = lookup(path);

	inodeToStat(oid, inode(oid), &st);
	return st;
}

std::shared_ptr<Reader> APFSVolume::openFile(const std::string& path)
{
	oid_t oid = lookup(path);
	std::vector<uint8_t> data = inode(oid);
	j_inode_val_t val;

	memcpy(&val, data.data(), sizeof(val));

	if (S_ISLNK(le(val.mode)))
	{
		std::string target = symlinkTarget(oid);
		return std::make_shared<MemoryReader>(reinterpret_cast<const uint8_t*>(target.data()), target.size());
	}
	if (le(val.bsd_flags) & UF_COMPRESSED)
	{
		std::vector<uint8_t> attribute;

		if (!xattr(oid, DECMPFS_XATTR_NAME, attribute))
			throw io_error("APFS compressed file has no decmpfs attribute: " + path);
		return openDecmpfsFile(attribute, [&]() { return xattrReader(oid, RESOURCE_FORK_EA_NAME); }, path);
	}

	return openStream(le(val.private_id), dstreamSize(data));
}
