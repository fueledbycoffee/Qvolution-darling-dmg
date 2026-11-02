#ifndef APFSBTREENODE_H
#define APFSBTREENODE_H
#include "apfs.h"
#include <stdint.h>
#include <vector>

// One node of an APFS B-tree, as read by APFSContainer::readObject().
// Every offset is checked against the node, so a damaged node throws io_error
// instead of reading outside the block.
class APFSBTreeNode
{
public:
	// Nodes whose entries have a fixed size take the sizes from their tree, which records them
	// in btree_info_t; trees with variable-size entries pass 0.
	APFSBTreeNode(std::vector<uint8_t> block, uint16_t fixedKeySize, uint16_t fixedValueSize);

	inline bool isLeaf() const { return m_flags & BTNODE_LEAF; }
	inline uint16_t level() const { return m_level; }
	inline uint32_t count() const { return m_count; }

	const uint8_t* key(uint32_t index, uint16_t* length) const;
	const uint8_t* value(uint32_t index, uint16_t* length) const;
	// The child that a nonleaf entry points to.
	oid_t child(uint32_t index) const;
private:
	void entry(uint32_t index, uint16_t* keyOffset, uint16_t* keyLength, uint16_t* valueOffset, uint16_t* valueLength) const;
private:
	std::vector<uint8_t> m_block;
	uint16_t m_flags, m_level;
	uint32_t m_count;
	uint32_t m_tableStart, m_keyStart, m_valueEnd;
	uint16_t m_fixedKeySize, m_fixedValueSize;
};

#endif
