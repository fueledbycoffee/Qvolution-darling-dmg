#ifndef APFSVOLUME_H
#define APFSVOLUME_H
#include "APFSContainer.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

// A read-only view of one unencrypted APFS volume, with the same interface as
// HFSHighLevelVolume. Files compressed with decmpfs are not supported yet and fail to open.
class APFSVolume
{
public:
	APFSVolume(std::shared_ptr<APFSContainer> container, oid_t volumeOid);

	// See exceptions.h for the list of possible exceptions
	std::map<std::string, struct stat> listDirectory(const std::string& path);
	std::shared_ptr<Reader> openFile(const std::string& path);
	struct stat stat(const std::string& path);
private:
	typedef std::function<void(const uint8_t* key, uint16_t keyLength, const uint8_t* value, uint16_t valueLength)> RecordVisitor;

	// Visits every file-system record of one object and record type, in key order.
	void forEachRecord(oid_t oid, uint8_t type, const RecordVisitor& visit);
	void visitNode(oid_t node, int expectedLevel, uint64_t target, const RecordVisitor& visit);

	oid_t lookup(const std::string& path);
	std::map<std::string, oid_t> directoryEntries(oid_t directory);
	std::vector<uint8_t> inode(oid_t oid);
	bool xattr(oid_t oid, const std::string& name, std::vector<uint8_t>& data);
	std::string symlinkTarget(oid_t oid);
	std::shared_ptr<Reader> openStream(oid_t streamId, uint64_t size);
	void inodeToStat(oid_t oid, const std::vector<uint8_t>& inode, struct stat* st);
private:
	std::shared_ptr<APFSContainer> m_container;
	paddr_t m_omap;
	oid_t m_rootTree;
	bool m_virtualTree;
};

#endif
