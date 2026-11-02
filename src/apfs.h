#ifndef APFS_H
#define APFS_H
#include <stdint.h>

// On-disk structures of the Apple File System, from Apple's "Apple File System Reference"
// (2020-06-22). All integers are little-endian. Only what a read-only reader of unencrypted
// volumes needs is declared; trailing fields that it never reads are left out.

#pragma pack(1)

typedef uint64_t oid_t;
typedef uint64_t xid_t;
typedef int64_t paddr_t;

#define MAX_CKSUM_SIZE 8

#define OBJECT_TYPE_MASK 0x0000ffff
#define OBJECT_TYPE_NX_SUPERBLOCK 0x00000001
#define OBJECT_TYPE_BTREE 0x00000002
#define OBJECT_TYPE_BTREE_NODE 0x00000003
#define OBJECT_TYPE_OMAP 0x0000000b
#define OBJECT_TYPE_FS 0x0000000d

#define OBJ_PHYSICAL 0x40000000

struct obj_phys_t
{
	uint8_t o_cksum[MAX_CKSUM_SIZE];
	oid_t o_oid;
	xid_t o_xid;
	uint32_t o_type;
	uint32_t o_subtype;
};

#define NX_MAGIC 0x4253584e // 'NXSB'
#define NX_MAX_FILE_SYSTEMS 100
#define NX_MINIMUM_BLOCK_SIZE 4096
#define NX_MAXIMUM_BLOCK_SIZE 65536

#define NX_INCOMPAT_VERSION2 0x0000000000000002ULL

struct nx_superblock_t
{
	obj_phys_t nx_o;
	uint32_t nx_magic;
	uint32_t nx_block_size;
	uint64_t nx_block_count;
	uint64_t nx_features;
	uint64_t nx_readonly_compatible_features;
	uint64_t nx_incompatible_features;
	uint8_t nx_uuid[16];
	oid_t nx_next_oid;
	xid_t nx_next_xid;
	uint32_t nx_xp_desc_blocks;
	uint32_t nx_xp_data_blocks;
	paddr_t nx_xp_desc_base;
	paddr_t nx_xp_data_base;
	uint32_t nx_xp_desc_next;
	uint32_t nx_xp_data_next;
	uint32_t nx_xp_desc_index;
	uint32_t nx_xp_desc_len;
	uint32_t nx_xp_data_index;
	uint32_t nx_xp_data_len;
	oid_t nx_spaceman_oid;
	oid_t nx_omap_oid;
	oid_t nx_reaper_oid;
	uint32_t nx_test_type;
	uint32_t nx_max_file_systems;
	oid_t nx_fs_oid[NX_MAX_FILE_SYSTEMS];
};

struct omap_phys_t
{
	obj_phys_t om_o;
	uint32_t om_flags;
	uint32_t om_snap_count;
	uint32_t om_tree_type;
	uint32_t om_snapshot_tree_type;
	oid_t om_tree_oid;
	oid_t om_snapshot_tree_oid;
	xid_t om_most_recent_snap;
	xid_t om_pending_revert_min;
	xid_t om_pending_revert_max;
};

struct omap_key_t
{
	oid_t ok_oid;
	xid_t ok_xid;
};

#define OMAP_VAL_DELETED 0x00000001
#define OMAP_VAL_ENCRYPTED 0x00000004

struct omap_val_t
{
	uint32_t ov_flags;
	uint32_t ov_size;
	paddr_t ov_paddr;
};

struct nloc_t
{
	uint16_t off;
	uint16_t len;
};

struct kvloc_t
{
	nloc_t k;
	nloc_t v;
};

struct kvoff_t
{
	uint16_t k;
	uint16_t v;
};

#define BTNODE_ROOT 0x0001
#define BTNODE_LEAF 0x0002
#define BTNODE_FIXED_KV_SIZE 0x0004

struct btree_node_phys_t
{
	obj_phys_t btn_o;
	uint16_t btn_flags;
	uint16_t btn_level;
	uint32_t btn_nkeys;
	nloc_t btn_table_space;
	nloc_t btn_free_space;
	nloc_t btn_key_free_list;
	nloc_t btn_val_free_list;
	uint8_t btn_data[0];
};

struct btree_info_t
{
	uint32_t bt_flags;
	uint32_t bt_node_size;
	uint32_t bt_key_size;
	uint32_t bt_val_size;
	uint32_t bt_longest_key;
	uint32_t bt_longest_val;
	uint64_t bt_key_count;
	uint64_t bt_node_count;
};

#define APFS_MAGIC 0x42535041 // 'APSB'

#define APFS_FS_UNENCRYPTED 0x00000001ULL

#define APFS_INCOMPAT_CASE_INSENSITIVE 0x00000001ULL
#define APFS_INCOMPAT_NORMALIZATION_INSENSITIVE 0x00000008ULL

struct wrapped_meta_crypto_state_t
{
	uint16_t major_version;
	uint16_t minor_version;
	uint32_t cpflags;
	uint32_t persistent_class;
	uint32_t key_os_version;
	uint16_t key_revision;
	uint16_t unused;
};

struct apfs_superblock_t
{
	obj_phys_t apfs_o;
	uint32_t apfs_magic;
	uint32_t apfs_fs_index;
	uint64_t apfs_features;
	uint64_t apfs_readonly_compatible_features;
	uint64_t apfs_incompatible_features;
	uint64_t apfs_unmount_time;
	uint64_t apfs_fs_reserve_block_count;
	uint64_t apfs_fs_quota_block_count;
	uint64_t apfs_fs_alloc_count;
	wrapped_meta_crypto_state_t apfs_meta_crypto;
	uint32_t apfs_root_tree_type;
	uint32_t apfs_extentref_tree_type;
	uint32_t apfs_snap_meta_tree_type;
	oid_t apfs_omap_oid;
	oid_t apfs_root_tree_oid;
	oid_t apfs_extentref_tree_oid;
	oid_t apfs_snap_meta_tree_oid;
	xid_t apfs_revert_to_xid;
	oid_t apfs_revert_to_sblock_oid;
	uint64_t apfs_next_obj_id;
	uint64_t apfs_num_files;
	uint64_t apfs_num_directories;
	uint64_t apfs_num_symlinks;
	uint64_t apfs_num_other_fsobjects;
	uint64_t apfs_num_snapshots;
	uint64_t apfs_total_blocks_alloced;
	uint64_t apfs_total_blocks_freed;
	uint8_t apfs_vol_uuid[16];
	uint64_t apfs_last_mod_time;
	uint64_t apfs_fs_flags;
};

#define OBJ_ID_MASK 0x0fffffffffffffffULL
#define OBJ_TYPE_SHIFT 60

enum
{
	APFS_TYPE_INODE = 3,
	APFS_TYPE_XATTR = 4,
	APFS_TYPE_FILE_EXTENT = 8,
	APFS_TYPE_DIR_REC = 9
};

#define ROOT_DIR_INO_NUM 2

struct j_key_t
{
	uint64_t obj_id_and_type;
};

struct j_inode_val_t
{
	uint64_t parent_id;
	uint64_t private_id;
	uint64_t create_time;
	uint64_t mod_time;
	uint64_t change_time;
	uint64_t access_time;
	uint64_t internal_flags;
	int32_t nchildren_or_nlink;
	uint32_t default_protection_class;
	uint32_t write_generation_counter;
	uint32_t bsd_flags;
	uint32_t owner;
	uint32_t group;
	uint16_t mode;
	uint16_t pad1;
	uint64_t uncompressed_size;
	uint8_t xfields[0];
};

struct xf_blob_t
{
	uint16_t xf_num_exts;
	uint16_t xf_used_data;
	uint8_t xf_data[0];
};

struct x_field_t
{
	uint8_t x_type;
	uint8_t x_flags;
	uint16_t x_size;
};

#define INO_EXT_TYPE_DSTREAM 8

struct j_dstream_t
{
	uint64_t size;
	uint64_t alloced_size;
	uint64_t default_crypto_id;
	uint64_t total_bytes_written;
	uint64_t total_bytes_read;
};

struct j_drec_key_t
{
	j_key_t hdr;
	uint16_t name_len;
	uint8_t name[0];
};

#define J_DREC_LEN_MASK 0x000003ff

struct j_drec_hashed_key_t
{
	j_key_t hdr;
	uint32_t name_len_and_hash;
	uint8_t name[0];
};

struct j_drec_val_t
{
	uint64_t file_id;
	uint64_t date_added;
	uint16_t flags;
	uint8_t xfields[0];
};

struct j_file_extent_key_t
{
	j_key_t hdr;
	uint64_t logical_addr;
};

#define J_FILE_EXTENT_LEN_MASK 0x00ffffffffffffffULL
#define J_FILE_EXTENT_FLAG_SHIFT 56

struct j_file_extent_val_t
{
	uint64_t len_and_flags;
	uint64_t phys_block_num;
	uint64_t crypto_id;
};

struct j_xattr_key_t
{
	j_key_t hdr;
	uint16_t name_len;
	uint8_t name[0];
};

#define XATTR_DATA_STREAM 0x0001
#define XATTR_DATA_EMBEDDED 0x0002

struct j_xattr_val_t
{
	uint16_t flags;
	uint16_t xdata_len;
	uint8_t xdata[0];
};

struct j_xattr_dstream_t
{
	uint64_t xattr_obj_id;
	j_dstream_t dstream;
};

#define SYMLINK_EA_NAME "com.apple.fs.symlink"

#pragma pack()

#endif
