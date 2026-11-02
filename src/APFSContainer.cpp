#include "APFSContainer.h"
#include "APFSBTreeNode.h"
#include "exceptions.h"
#include <algorithm>
#include <cstring>
#include <string>

// The highest bit of nx_xp_desc_blocks flags a descriptor area that is not contiguous.
static const uint32_t XP_DESC_NONCONTIGUOUS = 0x80000000;

APFSContainer::APFSContainer(std::shared_ptr<Reader> reader)
	: m_reader(reader), m_blockSize(NX_MINIMUM_BLOCK_SIZE)
{
	std::vector<uint8_t> block;
	const nx_superblock_t* sb;
	uint32_t descBlocks;
	xid_t bestXid = 0;

	// Block zero holds a copy of the superblock, which locates the checkpoint descriptor area.
	readBlock(0, block);
	sb = reinterpret_cast<const nx_superblock_t*>(block.data());
	if (le(sb->nx_magic) != NX_MAGIC)
		throw io_error("Not an APFS container");

	m_blockSize = le(sb->nx_block_size);
	if (m_blockSize < NX_MINIMUM_BLOCK_SIZE || m_blockSize > NX_MAXIMUM_BLOCK_SIZE || (m_blockSize & (m_blockSize - 1)))
		throw io_error("Invalid APFS block size: " + std::to_string(m_blockSize));
	readBlock(0, block);
	sb = reinterpret_cast<const nx_superblock_t*>(block.data());

	descBlocks = le(sb->nx_xp_desc_blocks);
	if (descBlocks & XP_DESC_NONCONTIGUOUS)
		throw function_not_implemented_error("Noncontiguous APFS checkpoint descriptor areas are not supported");

	// The latest checkpoint is the valid superblock with the highest transaction identifier.
	paddr_t descBase = le(sb->nx_xp_desc_base);
	for (uint32_t i = 0; i < descBlocks; i++)
	{
		std::vector<uint8_t> candidate;
		readBlock(descBase + i, candidate);
		const nx_superblock_t* csb = reinterpret_cast<const nx_superblock_t*>(candidate.data());

		if ((le(csb->nx_o.o_type) & OBJECT_TYPE_MASK) != OBJECT_TYPE_NX_SUPERBLOCK || le(csb->nx_magic) != NX_MAGIC
			|| !checksumValid(candidate) || le(csb->nx_block_size) != m_blockSize)
		{
			continue;
		}
		if (le(csb->nx_o.o_xid) > bestXid)
		{
			bestXid = le(csb->nx_o.o_xid);
			memcpy(&m_superblock, csb, sizeof(m_superblock));
		}
	}

	if (!bestXid)
		throw io_error("APFS container has no valid checkpoint");
	if (le(m_superblock.nx_incompatible_features) != NX_INCOMPAT_VERSION2)
		throw function_not_implemented_error("Unsupported APFS container features: " + std::to_string(le(m_superblock.nx_incompatible_features)));
}

bool APFSContainer::isAPFS(std::shared_ptr<Reader> reader)
{
	nx_superblock_t sb;

	if (reader->read(&sb, sizeof(sb), 0) != sizeof(sb))
		return false;
	return le(sb.nx_magic) == NX_MAGIC;
}

std::vector<oid_t> APFSContainer::volumes() const
{
	std::vector<oid_t> rv;
	uint32_t max = std::min<uint32_t>(le(m_superblock.nx_max_file_systems), NX_MAX_FILE_SYSTEMS);

	for (uint32_t i = 0; i < max; i++)
	{
		if (m_superblock.nx_fs_oid[i])
			rv.push_back(le(m_superblock.nx_fs_oid[i]));
	}
	return rv;
}

void APFSContainer::readBlock(paddr_t paddr, std::vector<uint8_t>& block)
{
	block.resize(m_blockSize);
	if (paddr < 0 || m_reader->read(block.data(), m_blockSize, uint64_t(paddr) * m_blockSize) != int32_t(m_blockSize))
		throw io_error("Cannot read APFS block " + std::to_string(paddr));
}

uint32_t APFSContainer::readObject(paddr_t paddr, std::vector<uint8_t>& block)
{
	readBlock(paddr, block);
	if (!checksumValid(block))
		throw io_error("APFS object at block " + std::to_string(paddr) + " fails its checksum");
	return le(reinterpret_cast<const obj_phys_t*>(block.data())->o_type) & OBJECT_TYPE_MASK;
}

// Fletcher-64 over the 32-bit words that follow o_cksum, stored as two words that make
// both running sums zero modulo 2^32-1.
bool APFSContainer::checksumValid(const std::vector<uint8_t>& block)
{
	const uint64_t mod = 0xffffffff;
	uint64_t sum1 = 0, sum2 = 0, low, high, stored;

	for (size_t i = MAX_CKSUM_SIZE; i + 4 <= block.size(); i += 4)
	{
		uint32_t word;
		memcpy(&word, &block[i], sizeof(word));
		sum1 = (sum1 + le(word)) % mod;
		sum2 = (sum2 + sum1) % mod;
	}

	low = mod - ((sum1 + sum2) % mod);
	high = mod - ((sum1 + low) % mod);
	memcpy(&stored, block.data(), sizeof(stored));
	return le(stored) == ((high << 32) | low);
}

paddr_t APFSContainer::resolve(paddr_t omapPaddr, oid_t oid)
{
	std::vector<uint8_t> block;
	const omap_phys_t* omap;
	paddr_t node;
	int expectedLevel = -1;
	xid_t xid = le(m_superblock.nx_o.o_xid);

	if (readObject(omapPaddr, block) != OBJECT_TYPE_OMAP)
		throw io_error("APFS object map expected at block " + std::to_string(omapPaddr));
	omap = reinterpret_cast<const omap_phys_t*>(block.data());
	if (!(le(omap->om_tree_type) & OBJ_PHYSICAL))
		throw io_error("APFS object map tree is not physical");
	node = le(omap->om_tree_oid);

	while (true)
	{
		uint32_t type = readObject(node, block);
		if (type != OBJECT_TYPE_BTREE && type != OBJECT_TYPE_BTREE_NODE)
			throw io_error("APFS B-tree node expected at block " + std::to_string(node));

		APFSBTreeNode btn(std::move(block), sizeof(omap_key_t), sizeof(omap_val_t));
		if (expectedLevel >= 0 && btn.level() != expectedLevel)
			throw io_error("APFS object map node has an unexpected level");

		// Entries are sorted by identifier, then transaction: take the last one at or before (oid, xid).
		int64_t found = -1;
		omap_key_t key;
		for (uint32_t i = 0; i < btn.count(); i++)
		{
			uint16_t length;
			const omap_key_t* k = reinterpret_cast<const omap_key_t*>(btn.key(i, &length));

			if (le(k->ok_oid) > oid || (le(k->ok_oid) == oid && le(k->ok_xid) > xid))
				break;
			found = i;
			key = *k;
		}
		if (found < 0 || (btn.isLeaf() && le(key.ok_oid) != oid))
			throw io_error("APFS object " + std::to_string(oid) + " is not in the object map");

		if (btn.isLeaf())
		{
			uint16_t length;
			const omap_val_t* val = reinterpret_cast<const omap_val_t*>(btn.value(found, &length));

			if (le(val->ov_flags) & OMAP_VAL_DELETED)
				throw io_error("APFS object " + std::to_string(oid) + " has been deleted");
			if (le(val->ov_flags) & OMAP_VAL_ENCRYPTED)
				throw function_not_implemented_error("Encrypted APFS objects are not supported");
			return le(val->ov_paddr);
		}

		node = btn.child(found);
		expectedLevel = btn.level() - 1;
	}
}
