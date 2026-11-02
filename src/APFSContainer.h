#ifndef APFSCONTAINER_H
#define APFSCONTAINER_H
#include "apfs.h"
#include "Reader.h"
#include "be.h"
#include <memory>
#include <vector>

// An APFS container (a partition formatted with APFS), mounted read-only from its latest
// valid checkpoint. Volumes are read with APFSVolume.
class APFSContainer
{
public:
	APFSContainer(std::shared_ptr<Reader> reader);

	static bool isAPFS(std::shared_ptr<Reader> reader);

	inline uint32_t blockSize() const { return m_blockSize; }
	// Number of blocks in the container; every valid physical address is below it.
	inline uint64_t blockCount() const { return m_blockCount; }
	inline std::shared_ptr<Reader> reader() { return m_reader; }

	// Virtual object identifiers of the container's volumes.
	std::vector<oid_t> volumes() const;
	// Physical address of the object map that resolves the volume identifiers.
	inline paddr_t objectMap() const { return le(m_superblock.nx_omap_oid); }

	// Reads the object at a physical address, verifies its checksum and returns its type.
	uint32_t readObject(paddr_t paddr, std::vector<uint8_t>& block);
	// Resolves a virtual object identifier through the object map at omapPaddr, as of the
	// mounted checkpoint.
	paddr_t resolve(paddr_t omapPaddr, oid_t oid);
private:
	void readBlock(paddr_t paddr, std::vector<uint8_t>& block);
	void setBlockCount(uint64_t blockCount);
	static bool checksumValid(const std::vector<uint8_t>& block);
private:
	std::shared_ptr<Reader> m_reader;
	uint32_t m_blockSize;
	uint64_t m_blockCount;
	nx_superblock_t m_superblock;
};

#endif
