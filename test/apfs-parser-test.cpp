// Builds a minimal APFS container in memory, from the structures in apfs.h, and checks that
// APFSContainer and APFSVolume read it, then that they reject each kind of damage below
// instead of returning wrong data.
//
// Blocks: 0 superblock copy, 1 checkpoint superblock, 2-3 container object map, 4 volume
// superblock, 5-6 volume object map, 7 file-system tree root, 8 file data, 9 spare node.
#include "APFSContainer.h"
#include "APFSVolume.h"
#include "MemoryReader.h"
#include "exceptions.h"
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

static const uint32_t BS = 4096;
static const uint64_t BLOCKS = 10;
static const oid_t VOLUME_OID = 1026, ROOT_TREE_OID = 1027, SPARE_NODE_OID = 1028;
static const char CONTENT[] = "hello world\n";

static int failures = 0;

static void expect(bool ok, const std::string& what)
{
	std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
	if (!ok)
		failures++;
}

struct Options
{
	bool omapFixed = true;
	uint64_t extentLogical = 0, extentLength = BS, extentBlock = 8, extentFlags = 0;
	bool overlappingExtent = false;
	bool breakBlockZeroChecksum = false;
	int64_t descBase = 1;
	uint64_t blockCount = BLOCKS;
	bool truncatedInodeTail = false;
	std::string fileName = "hello.txt";
	bool selfReferencingTree = false;
	bool treeRevisitsNode = false;
};

struct Entry
{
	std::vector<uint8_t> key, value;
};

template <typename T> static void append(std::vector<uint8_t>& v, const T& x)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
	v.insert(v.end(), p, p + sizeof(x));
}

static std::vector<uint8_t> fsKey(oid_t oid, uint64_t type)
{
	std::vector<uint8_t> k;
	append(k, j_key_t{ (type << OBJ_TYPE_SHIFT) | oid });
	return k;
}

static void seal(uint8_t* block)
{
	const uint64_t mod = 0xffffffff;
	uint64_t sum1 = 0, sum2 = 0;

	for (size_t i = MAX_CKSUM_SIZE; i < BS; i += 4)
	{
		uint32_t word;
		memcpy(&word, block + i, sizeof(word));
		sum1 = (sum1 + word) % mod;
		sum2 = (sum2 + sum1) % mod;
	}
	uint64_t low = mod - ((sum1 + sum2) % mod);
	uint64_t high = mod - ((sum1 + low) % mod);
	uint64_t cksum = (high << 32) | low;
	memcpy(block, &cksum, sizeof(cksum));
}

static void writeNode(uint8_t* block, uint32_t type, bool root, uint16_t level, bool fixed,
	uint32_t keySize, uint32_t valueSize, const std::vector<Entry>& entries)
{
	btree_node_phys_t node;
	memset(&node, 0, sizeof(node));
	node.btn_o.o_type = type;
	node.btn_flags = (root ? BTNODE_ROOT : 0) | (level == 0 ? BTNODE_LEAF : 0) | (fixed ? BTNODE_FIXED_KV_SIZE : 0);
	node.btn_level = level;
	node.btn_nkeys = entries.size();
	node.btn_table_space.len = entries.size() * (fixed ? sizeof(kvoff_t) : sizeof(kvloc_t));
	memcpy(block, &node, sizeof(node));

	uint32_t keyStart = sizeof(node) + node.btn_table_space.len;
	uint32_t valueEnd = BS - (root ? sizeof(btree_info_t) : 0);
	uint16_t keyOffset = 0, valueOffset = 0;

	for (size_t i = 0; i < entries.size(); i++)
	{
		const Entry& e = entries[i];
		memcpy(block + keyStart + keyOffset, e.key.data(), e.key.size());
		valueOffset += e.value.size();
		memcpy(block + valueEnd - valueOffset, e.value.data(), e.value.size());

		if (fixed)
		{
			kvoff_t toc = { keyOffset, valueOffset };
			memcpy(block + sizeof(node) + i * sizeof(toc), &toc, sizeof(toc));
		}
		else
		{
			kvloc_t toc = { { keyOffset, uint16_t(e.key.size()) }, { valueOffset, uint16_t(e.value.size()) } };
			memcpy(block + sizeof(node) + i * sizeof(toc), &toc, sizeof(toc));
		}
		keyOffset += e.key.size();
	}

	if (root)
	{
		btree_info_t info;
		memset(&info, 0, sizeof(info));
		info.bt_node_size = BS;
		info.bt_key_size = keySize;
		info.bt_val_size = valueSize;
		info.bt_key_count = entries.size();
		info.bt_node_count = 1;
		memcpy(block + BS - sizeof(info), &info, sizeof(info));
	}
}

static Entry omapEntry(oid_t oid, paddr_t paddr)
{
	Entry e;
	append(e.key, omap_key_t{ oid, 1 });
	append(e.value, omap_val_t{ 0, BS, paddr });
	return e;
}

static std::vector<uint8_t> inodeValue(oid_t parent, oid_t id, uint16_t mode, int32_t links)
{
	j_inode_val_t inode;
	memset(&inode, 0, sizeof(inode));
	inode.parent_id = parent;
	inode.private_id = id;
	inode.nchildren_or_nlink = links;
	inode.mode = mode;
	std::vector<uint8_t> v;
	append(v, inode);
	return v;
}

static Entry drec(oid_t parent, const std::string& name, oid_t file, uint16_t type)
{
	Entry e = { fsKey(parent, APFS_TYPE_DIR_REC), {} };
	append(e.key, uint32_t(name.size() + 1)); // j_drec_hashed_key_t, hash left zero
	e.key.insert(e.key.end(), name.begin(), name.end());
	e.key.push_back(0);
	append(e.value, j_drec_val_t{ file, 0, type });
	return e;
}

static Entry extent(oid_t stream, uint64_t logical, uint64_t length, uint64_t block, uint64_t flags)
{
	Entry e = { fsKey(stream, APFS_TYPE_FILE_EXTENT), {} };
	append(e.key, logical);
	append(e.value, j_file_extent_val_t{ length | (flags << J_FILE_EXTENT_FLAG_SHIFT), block, 0 });
	return e;
}

static std::vector<Entry> fsRecords(const Options& o)
{
	std::vector<Entry> records;

	records.push_back({ fsKey(ROOT_DIR_INO_NUM, APFS_TYPE_INODE), inodeValue(1, ROOT_DIR_INO_NUM, S_IFDIR | 0755, 2) });
	records.push_back(drec(ROOT_DIR_INO_NUM, o.fileName, 16, 8));
	records.push_back(drec(ROOT_DIR_INO_NUM, "link", 17, 10));

	// The file's inode carries its size in an INO_EXT_TYPE_DSTREAM extended field.
	std::vector<uint8_t> file = inodeValue(ROOT_DIR_INO_NUM, 16, S_IFREG | 0644, 1);
	if (o.truncatedInodeTail)
		file.push_back(1);
	else
	{
		j_dstream_t dstream;
		memset(&dstream, 0, sizeof(dstream));
		dstream.size = sizeof(CONTENT) - 1;
		dstream.alloced_size = BS;
		append(file, xf_blob_t{ 1, sizeof(dstream) });
		append(file, x_field_t{ INO_EXT_TYPE_DSTREAM, 0, sizeof(dstream) });
		append(file, dstream);
	}
	records.push_back({ fsKey(16, APFS_TYPE_INODE), file });
	records.push_back(extent(16, o.extentLogical, o.extentLength, o.extentBlock, o.extentFlags));
	if (o.overlappingExtent)
		records.push_back(extent(16, o.extentLogical + BS / 2, BS, o.extentBlock, 0));

	records.push_back({ fsKey(17, APFS_TYPE_INODE), inodeValue(ROOT_DIR_INO_NUM, 17, S_IFLNK | 0755, 1) });
	Entry target = { fsKey(17, APFS_TYPE_XATTR), {} };
	const std::string name = SYMLINK_EA_NAME, path = "hello.txt";
	append(target.key, uint16_t(name.size() + 1));
	target.key.insert(target.key.end(), name.begin(), name.end());
	target.key.push_back(0);
	append(target.value, j_xattr_val_t{ XATTR_DATA_EMBEDDED, uint16_t(path.size() + 1) });
	target.value.insert(target.value.end(), path.begin(), path.end());
	target.value.push_back(0);
	records.push_back(target);

	return records;
}

static std::vector<uint8_t> build(const Options& o)
{
	std::vector<uint8_t> image(BLOCKS * BS);
	auto block = [&](size_t n) { return &image[n * BS]; };

	nx_superblock_t nx;
	memset(&nx, 0, sizeof(nx));
	nx.nx_o.o_oid = 1;
	nx.nx_o.o_xid = 1;
	nx.nx_o.o_type = OBJECT_TYPE_NX_SUPERBLOCK;
	nx.nx_magic = NX_MAGIC;
	nx.nx_block_size = BS;
	nx.nx_block_count = o.blockCount;
	nx.nx_incompatible_features = NX_INCOMPAT_VERSION2;
	nx.nx_xp_desc_blocks = 1;
	nx.nx_xp_desc_base = o.descBase;
	nx.nx_omap_oid = 2;
	nx.nx_max_file_systems = NX_MAX_FILE_SYSTEMS;
	nx.nx_fs_oid[0] = VOLUME_OID;
	memcpy(block(0), &nx, sizeof(nx));
	memcpy(block(1), &nx, sizeof(nx));

	for (paddr_t omap : { 2, 5 })
	{
		omap_phys_t om;
		memset(&om, 0, sizeof(om));
		om.om_o.o_oid = omap;
		om.om_o.o_type = OBJECT_TYPE_OMAP | OBJ_PHYSICAL;
		om.om_tree_type = OBJECT_TYPE_BTREE | OBJ_PHYSICAL;
		om.om_tree_oid = omap + 1;
		memcpy(block(omap), &om, sizeof(om));
	}
	writeNode(block(3), OBJECT_TYPE_BTREE | OBJ_PHYSICAL, true, 0, o.omapFixed, sizeof(omap_key_t), sizeof(omap_val_t),
		{ omapEntry(VOLUME_OID, 4) });
	writeNode(block(6), OBJECT_TYPE_BTREE | OBJ_PHYSICAL, true, 0, true, sizeof(omap_key_t), sizeof(omap_val_t),
		{ omapEntry(ROOT_TREE_OID, 7), omapEntry(SPARE_NODE_OID, 9) });

	apfs_superblock_t apsb;
	memset(&apsb, 0, sizeof(apsb));
	apsb.apfs_o.o_oid = VOLUME_OID;
	apsb.apfs_o.o_xid = 1;
	apsb.apfs_o.o_type = OBJECT_TYPE_FS;
	apsb.apfs_magic = APFS_MAGIC;
	apsb.apfs_root_tree_type = OBJECT_TYPE_BTREE; // virtual, resolved through the volume object map
	apsb.apfs_omap_oid = 5;
	apsb.apfs_root_tree_oid = ROOT_TREE_OID;
	apsb.apfs_fs_flags = APFS_FS_UNENCRYPTED;
	memcpy(block(4), &apsb, sizeof(apsb));

	if (o.selfReferencingTree || o.treeRevisitsNode)
	{
		// An index root whose entries point back at itself, or many times at one leaf.
		Entry e = { fsKey(ROOT_DIR_INO_NUM, APFS_TYPE_INODE), {} };
		append(e.value, o.selfReferencingTree ? ROOT_TREE_OID : SPARE_NODE_OID);
		writeNode(block(7), OBJECT_TYPE_BTREE, true, 1, false, 0, 0, std::vector<Entry>(o.selfReferencingTree ? 1 : 12, e));
		writeNode(block(9), OBJECT_TYPE_BTREE_NODE, false, 0, false, 0, 0, fsRecords(o));
	}
	else
		writeNode(block(7), OBJECT_TYPE_BTREE, true, 0, false, 0, 0, fsRecords(o));

	memcpy(block(8), CONTENT, sizeof(CONTENT) - 1);

	for (size_t n = 0; n < BLOCKS; n++)
	{
		if (n != 8)
			seal(block(n));
	}
	if (o.breakBlockZeroChecksum)
		block(0)[100] ^= 1;
	return image;
}

static std::string readAll(std::shared_ptr<Reader> reader)
{
	std::string data(reader->length(), '\0');
	if (!data.empty() && reader->read(&data[0], data.size(), 0) != int32_t(data.size()))
		throw std::runtime_error("short read");
	return data;
}

// Mounts the image's volume and reads every entry of its root directory.
static std::map<std::string, std::string> extract(const std::vector<uint8_t>& image)
{
	auto container = std::make_shared<APFSContainer>(std::make_shared<MemoryReader>(image.data(), image.size()));
	APFSVolume volume(container, container->volumes().at(0));
	std::map<std::string, std::string> files;

	for (const auto& entry : volume.listDirectory("/"))
		files[entry.first] = readAll(volume.openFile("/" + entry.first));
	return files;
}

static void expectRejected(const std::string& what, std::function<void(Options&)> damage)
{
	Options o;
	damage(o);
	try
	{
		extract(build(o));
		expect(false, what + " is rejected (it was accepted)");
	}
	catch (const io_error& e)
	{
		expect(true, what + " is rejected: " + e.what());
	}
}

int main()
{
	std::map<std::string, std::string> files = extract(build(Options()));
	expect(files.size() == 2 && files["hello.txt"] == CONTENT && files["link"] == "hello.txt",
		"an intact container reads back its file and symbolic link");

	expectRejected("an object map node with variable-size entries", [](Options& o) { o.omapFixed = false; });
	expectRejected("a superblock copy in block zero with a bad checksum", [](Options& o) { o.breakBlockZeroChecksum = true; });
	expectRejected("a checkpoint area outside the container", [](Options& o) { o.descBase = 50; });
	expectRejected("a container larger than its partition", [](Options& o) { o.blockCount = BLOCKS + 1; });
	expectRejected("an extent starting outside the container", [](Options& o) { o.extentBlock = BLOCKS; });
	expectRejected("an extent running past the container", [](Options& o) { o.extentBlock = BLOCKS - 1; o.extentLength = 2 * BS; });
	expectRejected("an extent that is not whole blocks", [](Options& o) { o.extentLength = 100; });
	expectRejected("an extent past the largest file offset", [](Options& o) { o.extentLogical = UINT64_MAX - BS + 2; });
	expectRejected("an extent with flags", [](Options& o) { o.extentFlags = 1; });
	expectRejected("overlapping extents", [](Options& o) { o.overlappingExtent = true; });
	expectRejected("an inode with a truncated extended-field header", [](Options& o) { o.truncatedInodeTail = true; });
	expectRejected("a file name with a NUL in it", [](Options& o) { o.fileName = std::string("a\0b", 3); });
	expectRejected("a tree node that points at itself", [](Options& o) { o.selfReferencingTree = true; });
	expectRejected("a tree that reads one node more times than there are blocks", [](Options& o) { o.treeRevisitsNode = true; });

	return failures ? 1 : 0;
}
