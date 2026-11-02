#include "APFSBTreeNode.h"
#include "be.h"
#include "exceptions.h"
#include <cstring>

APFSBTreeNode::APFSBTreeNode(std::vector<uint8_t> block, uint16_t fixedKeySize, uint16_t fixedValueSize)
	: m_block(std::move(block)), m_fixedKeySize(fixedKeySize), m_fixedValueSize(fixedValueSize)
{
	const btree_node_phys_t* node = reinterpret_cast<const btree_node_phys_t*>(m_block.data());

	if (m_block.size() < sizeof(btree_node_phys_t) + sizeof(btree_info_t))
		throw io_error("APFS B-tree node is too small");

	m_flags = le(node->btn_flags);
	m_level = le(node->btn_level);
	m_count = le(node->btn_nkeys);

	if ((m_flags & BTNODE_FIXED_KV_SIZE) && (!m_fixedKeySize || !m_fixedValueSize))
		throw io_error("APFS B-tree node has fixed-size entries in a tree without them");
	if (isLeaf() != (m_level == 0))
		throw io_error("APFS B-tree node level does not match its leaf flag");

	// The table of contents starts at btn_data; keys follow it, and values are laid out backwards
	// from the end of the node, or from the btree_info_t that ends a root node.
	m_tableStart = sizeof(btree_node_phys_t) + le(node->btn_table_space.off);
	m_keyStart = m_tableStart + le(node->btn_table_space.len);
	m_valueEnd = m_block.size() - ((m_flags & BTNODE_ROOT) ? sizeof(btree_info_t) : 0);

	size_t entrySize = (m_flags & BTNODE_FIXED_KV_SIZE) ? sizeof(kvoff_t) : sizeof(kvloc_t);
	if (m_keyStart > m_valueEnd || uint64_t(m_count) * entrySize > m_keyStart - m_tableStart)
		throw io_error("APFS B-tree node table of contents is out of bounds");
}

void APFSBTreeNode::entry(uint32_t index, uint16_t* keyOffset, uint16_t* keyLength, uint16_t* valueOffset, uint16_t* valueLength) const
{
	if (index >= m_count)
		throw io_error("APFS B-tree entry index is out of bounds");

	if (m_flags & BTNODE_FIXED_KV_SIZE)
	{
		const kvoff_t* toc = reinterpret_cast<const kvoff_t*>(&m_block[m_tableStart]);
		*keyOffset = le(toc[index].k);
		*keyLength = m_fixedKeySize;
		*valueOffset = le(toc[index].v);
		*valueLength = isLeaf() ? m_fixedValueSize : sizeof(oid_t);
	}
	else
	{
		const kvloc_t* toc = reinterpret_cast<const kvloc_t*>(&m_block[m_tableStart]);
		*keyOffset = le(toc[index].k.off);
		*keyLength = le(toc[index].k.len);
		*valueOffset = le(toc[index].v.off);
		*valueLength = le(toc[index].v.len);
	}

	if (uint32_t(*keyOffset) + *keyLength > m_valueEnd - m_keyStart
		|| *valueOffset > m_valueEnd - m_keyStart || *valueLength > *valueOffset)
	{
		throw io_error("APFS B-tree entry is out of bounds");
	}
}

const uint8_t* APFSBTreeNode::key(uint32_t index, uint16_t* length) const
{
	uint16_t keyOffset, valueOffset, valueLength;

	entry(index, &keyOffset, length, &valueOffset, &valueLength);
	return &m_block[m_keyStart + keyOffset];
}

const uint8_t* APFSBTreeNode::value(uint32_t index, uint16_t* length) const
{
	uint16_t keyOffset, keyLength, valueOffset;

	entry(index, &keyOffset, &keyLength, &valueOffset, length);
	return &m_block[m_valueEnd - valueOffset];
}

oid_t APFSBTreeNode::child(uint32_t index) const
{
	uint16_t length;
	const uint8_t* data = value(index, &length);
	oid_t oid;

	if (isLeaf() || length < sizeof(oid))
		throw io_error("APFS B-tree entry is not a child pointer");

	memcpy(&oid, data, sizeof(oid));
	return le(oid);
}
