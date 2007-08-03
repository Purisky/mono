/*
 * image.c: Routines for manipulating an image stored in an
 * extended PE/COFF file.
 * 
 * Authors:
 *   Miguel de Icaza (miguel@ximian.com)
 *   Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2001-2003 Ximian, Inc.  http://www.ximian.com
 *
 */
#include <config.h>
#include <stdio.h>
#include <glib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include "image.h"
#include "cil-coff.h"
#include "rawbuffer.h"
#include "mono-endian.h"
#include "tabledefs.h"
#include "tokentype.h"
#include "metadata-internals.h"
#include "loader.h"
#include <mono/io-layer/io-layer.h>
#include <mono/utils/mono-logger.h>
#include <mono/utils/mono-path.h>
#include <mono/metadata/class-internals.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#define INVALID_ADDRESS 0xffffffff

/*
 * Keeps track of the various assemblies loaded
 */
static GHashTable *loaded_images_hash;
static GHashTable *loaded_images_guid_hash;
static GHashTable *loaded_images_refonly_hash;
static GHashTable *loaded_images_refonly_guid_hash;

static gboolean debug_assembly_unload = FALSE;

#define mono_images_lock() EnterCriticalSection (&images_mutex)
#define mono_images_unlock() LeaveCriticalSection (&images_mutex)
static CRITICAL_SECTION images_mutex;

guint32
mono_cli_rva_image_map (MonoCLIImageInfo *iinfo, guint32 addr)
{
	const int top = iinfo->cli_section_count;
	MonoSectionTable *tables = iinfo->cli_section_tables;
	int i;
	
	for (i = 0; i < top; i++){
		if ((addr >= tables->st_virtual_address) &&
		    (addr < tables->st_virtual_address + tables->st_raw_data_size)){
			return addr - tables->st_virtual_address + tables->st_raw_data_ptr;
		}
		tables++;
	}
	return INVALID_ADDRESS;
}

/**
 * mono_images_rva_map:
 * @image: a MonoImage
 * @addr: relative virtual address (RVA)
 *
 * This is a low-level routine used by the runtime to map relative
 * virtual address (RVA) into their location in memory. 
 *
 * Returns: the address in memory for the given RVA, or NULL if the
 * RVA is not valid for this image. 
 */
char *
mono_image_rva_map (MonoImage *image, guint32 addr)
{
	MonoCLIImageInfo *iinfo = image->image_info;
	const int top = iinfo->cli_section_count;
	MonoSectionTable *tables = iinfo->cli_section_tables;
	int i;
	
	for (i = 0; i < top; i++){
		if ((addr >= tables->st_virtual_address) &&
		    (addr < tables->st_virtual_address + tables->st_raw_data_size)){
			if (!iinfo->cli_sections [i]) {
				if (!mono_image_ensure_section_idx (image, i))
					return NULL;
			}
			return (char*)iinfo->cli_sections [i] +
				(addr - tables->st_virtual_address);
		}
		tables++;
	}
	return NULL;
}

/**
 * mono_images_init:
 *
 *  Initialize the global variables used by this module.
 */
void
mono_images_init (void)
{
	InitializeCriticalSection (&images_mutex);

	loaded_images_hash = g_hash_table_new (g_str_hash, g_str_equal);
	loaded_images_guid_hash = g_hash_table_new (g_str_hash, g_str_equal);
	loaded_images_refonly_hash = g_hash_table_new (g_str_hash, g_str_equal);
	loaded_images_refonly_guid_hash = g_hash_table_new (g_str_hash, g_str_equal);

	debug_assembly_unload = getenv ("MONO_DEBUG_ASSEMBLY_UNLOAD") != NULL;
}

/**
 * mono_images_cleanup:
 *
 *  Free all resources used by this module.
 */
void
mono_images_cleanup (void)
{
	DeleteCriticalSection (&images_mutex);

	g_hash_table_destroy (loaded_images_hash);
	g_hash_table_destroy (loaded_images_guid_hash);
	g_hash_table_destroy (loaded_images_refonly_hash);
	g_hash_table_destroy (loaded_images_refonly_guid_hash);
}

/**
 * mono_image_ensure_section_idx:
 * @image: The image we are operating on
 * @section: section number that we will load/map into memory
 *
 * This routine makes sure that we have an in-memory copy of
 * an image section (.text, .rsrc, .data).
 *
 * Returns: TRUE on success
 */
int
mono_image_ensure_section_idx (MonoImage *image, int section)
{
	MonoCLIImageInfo *iinfo = image->image_info;
	MonoSectionTable *sect;
	gboolean writable;
	
	g_return_val_if_fail (section < iinfo->cli_section_count, FALSE);

	if (iinfo->cli_sections [section] != NULL)
		return TRUE;

	sect = &iinfo->cli_section_tables [section];
	
	writable = sect->st_flags & SECT_FLAGS_MEM_WRITE;

	if (sect->st_raw_data_ptr + sect->st_raw_data_size > image->raw_data_len)
		return FALSE;
	/* FIXME: we ignore the writable flag since we don't patch the binary */
	iinfo->cli_sections [section] = image->raw_data + sect->st_raw_data_ptr;
	return TRUE;
}

/**
 * mono_image_ensure_section:
 * @image: The image we are operating on
 * @section: section name that we will load/map into memory
 *
 * This routine makes sure that we have an in-memory copy of
 * an image section (.text, .rsrc, .data).
 *
 * Returns: TRUE on success
 */
int
mono_image_ensure_section (MonoImage *image, const char *section)
{
	MonoCLIImageInfo *ii = image->image_info;
	int i;
	
	for (i = 0; i < ii->cli_section_count; i++){
		if (strncmp (ii->cli_section_tables [i].st_name, section, 8) != 0)
			continue;
		
		return mono_image_ensure_section_idx (image, i);
	}
	return FALSE;
}

static int
load_section_tables (MonoImage *image, MonoCLIImageInfo *iinfo, guint32 offset)
{
	const int top = iinfo->cli_header.coff.coff_sections;
	int i;

	iinfo->cli_section_count = top;
	iinfo->cli_section_tables = g_new0 (MonoSectionTable, top);
	iinfo->cli_sections = g_new0 (void *, top);
	
	for (i = 0; i < top; i++){
		MonoSectionTable *t = &iinfo->cli_section_tables [i];

		if (offset + sizeof (MonoSectionTable) > image->raw_data_len)
			return FALSE;
		memcpy (t, image->raw_data + offset, sizeof (MonoSectionTable));
		offset += sizeof (MonoSectionTable);

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
		t->st_virtual_size = GUINT32_FROM_LE (t->st_virtual_size);
		t->st_virtual_address = GUINT32_FROM_LE (t->st_virtual_address);
		t->st_raw_data_size = GUINT32_FROM_LE (t->st_raw_data_size);
		t->st_raw_data_ptr = GUINT32_FROM_LE (t->st_raw_data_ptr);
		t->st_reloc_ptr = GUINT32_FROM_LE (t->st_reloc_ptr);
		t->st_lineno_ptr = GUINT32_FROM_LE (t->st_lineno_ptr);
		t->st_reloc_count = GUINT16_FROM_LE (t->st_reloc_count);
		t->st_line_count = GUINT16_FROM_LE (t->st_line_count);
		t->st_flags = GUINT32_FROM_LE (t->st_flags);
#endif
		/* consistency checks here */
	}

	return TRUE;
}

static gboolean
load_cli_header (MonoImage *image, MonoCLIImageInfo *iinfo)
{
	guint32 offset;
	
	offset = mono_cli_rva_image_map (iinfo, iinfo->cli_header.datadir.pe_cli_header.rva);
	if (offset == INVALID_ADDRESS)
		return FALSE;

	if (offset + sizeof (MonoCLIHeader) > image->raw_data_len)
		return FALSE;
	memcpy (&iinfo->cli_cli_header, image->raw_data + offset, sizeof (MonoCLIHeader));

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#define SWAP32(x) (x) = GUINT32_FROM_LE ((x))
#define SWAP16(x) (x) = GUINT16_FROM_LE ((x))
#define SWAPPDE(x) do { (x).rva = GUINT32_FROM_LE ((x).rva); (x).size = GUINT32_FROM_LE ((x).size);} while (0)
	SWAP32 (iinfo->cli_cli_header.ch_size);
	SWAP32 (iinfo->cli_cli_header.ch_flags);
	SWAP32 (iinfo->cli_cli_header.ch_entry_point);
	SWAP16 (iinfo->cli_cli_header.ch_runtime_major);
	SWAP16 (iinfo->cli_cli_header.ch_runtime_minor);
	SWAPPDE (iinfo->cli_cli_header.ch_metadata);
	SWAPPDE (iinfo->cli_cli_header.ch_resources);
	SWAPPDE (iinfo->cli_cli_header.ch_strong_name);
	SWAPPDE (iinfo->cli_cli_header.ch_code_manager_table);
	SWAPPDE (iinfo->cli_cli_header.ch_vtable_fixups);
	SWAPPDE (iinfo->cli_cli_header.ch_export_address_table_jumps);
	SWAPPDE (iinfo->cli_cli_header.ch_eeinfo_table);
	SWAPPDE (iinfo->cli_cli_header.ch_helper_table);
	SWAPPDE (iinfo->cli_cli_header.ch_dynamic_info);
	SWAPPDE (iinfo->cli_cli_header.ch_delay_load_info);
	SWAPPDE (iinfo->cli_cli_header.ch_module_image);
	SWAPPDE (iinfo->cli_cli_header.ch_external_fixups);
	SWAPPDE (iinfo->cli_cli_header.ch_ridmap);
	SWAPPDE (iinfo->cli_cli_header.ch_debug_map);
	SWAPPDE (iinfo->cli_cli_header.ch_ip_map);
#undef SWAP32
#undef SWAP16
#undef SWAPPDE
#endif
	/* Catch new uses of the fields that are supposed to be zero */

	if ((iinfo->cli_cli_header.ch_eeinfo_table.rva != 0) ||
	    (iinfo->cli_cli_header.ch_helper_table.rva != 0) ||
	    (iinfo->cli_cli_header.ch_dynamic_info.rva != 0) ||
	    (iinfo->cli_cli_header.ch_delay_load_info.rva != 0) ||
	    (iinfo->cli_cli_header.ch_module_image.rva != 0) ||
	    (iinfo->cli_cli_header.ch_external_fixups.rva != 0) ||
	    (iinfo->cli_cli_header.ch_ridmap.rva != 0) ||
	    (iinfo->cli_cli_header.ch_debug_map.rva != 0) ||
	    (iinfo->cli_cli_header.ch_ip_map.rva != 0)){

		/*
		 * No need to scare people who are testing this, I am just
		 * labelling this as a LAMESPEC
		 */
		/* g_warning ("Some fields in the CLI header which should have been zero are not zero"); */

	}
	    
	return TRUE;
}

static gboolean
load_metadata_ptrs (MonoImage *image, MonoCLIImageInfo *iinfo)
{
	guint32 offset, size;
	guint16 streams;
	int i;
	guint32 pad;
	char *ptr;
	
	offset = mono_cli_rva_image_map (iinfo, iinfo->cli_cli_header.ch_metadata.rva);
	if (offset == INVALID_ADDRESS)
		return FALSE;

	size = iinfo->cli_cli_header.ch_metadata.size;

	if (offset + size > image->raw_data_len)
		return FALSE;
	image->raw_metadata = image->raw_data + offset;

	ptr = image->raw_metadata;

	if (strncmp (ptr, "BSJB", 4) == 0){
		guint32 version_string_len;

		ptr += 4;
		image->md_version_major = read16 (ptr);
		ptr += 4;
		image->md_version_minor = read16 (ptr);
		ptr += 4;

		version_string_len = read32 (ptr);
		ptr += 4;
		image->version = g_strndup (ptr, version_string_len);
		ptr += version_string_len;
		pad = ptr - image->raw_metadata;
		if (pad % 4)
			ptr += 4 - (pad % 4);
	} else
		return FALSE;

	/* skip over flags */
	ptr += 2;
	
	streams = read16 (ptr);
	ptr += 2;

	for (i = 0; i < streams; i++){
		if (strncmp (ptr + 8, "#~", 3) == 0){
			image->heap_tables.data = image->raw_metadata + read32 (ptr);
			image->heap_tables.size = read32 (ptr + 4);
			ptr += 8 + 3;
		} else if (strncmp (ptr + 8, "#Strings", 9) == 0){
			image->heap_strings.data = image->raw_metadata + read32 (ptr);
			image->heap_strings.size = read32 (ptr + 4);
			ptr += 8 + 9;
		} else if (strncmp (ptr + 8, "#US", 4) == 0){
			image->heap_us.data = image->raw_metadata + read32 (ptr);
			image->heap_us.size = read32 (ptr + 4);
			ptr += 8 + 4;
		} else if (strncmp (ptr + 8, "#Blob", 6) == 0){
			image->heap_blob.data = image->raw_metadata + read32 (ptr);
			image->heap_blob.size = read32 (ptr + 4);
			ptr += 8 + 6;
		} else if (strncmp (ptr + 8, "#GUID", 6) == 0){
			image->heap_guid.data = image->raw_metadata + read32 (ptr);
			image->heap_guid.size = read32 (ptr + 4);
			ptr += 8 + 6;
		} else if (strncmp (ptr + 8, "#-", 3) == 0) {
			image->heap_tables.data = image->raw_metadata + read32 (ptr);
			image->heap_tables.size = read32 (ptr + 4);
			ptr += 8 + 3;
			image->uncompressed_metadata = TRUE;
			mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Assembly '%s' has the non-standard metadata heap #-.\nRecompile it correctly (without the /incremental switch or in Release mode).\n", image->name);
		} else {
			g_message ("Unknown heap type: %s\n", ptr + 8);
			ptr += 8 + strlen (ptr + 8) + 1;
		}
		pad = ptr - image->raw_metadata;
		if (pad % 4)
			ptr += 4 - (pad % 4);
	}

	g_assert (image->heap_guid.data);
	g_assert (image->heap_guid.size >= 16);

	image->guid = mono_guid_to_string ((guint8*)image->heap_guid.data);

	return TRUE;
}

/*
 * Load representation of logical metadata tables, from the "#~" stream
 */
static gboolean
load_tables (MonoImage *image)
{
	const char *heap_tables = image->heap_tables.data;
	const guint32 *rows;
	guint64 valid_mask, sorted_mask;
	int valid = 0, table;
	int heap_sizes;
	
	heap_sizes = heap_tables [6];
	image->idx_string_wide = ((heap_sizes & 0x01) == 1);
	image->idx_guid_wide   = ((heap_sizes & 0x02) == 2);
	image->idx_blob_wide   = ((heap_sizes & 0x04) == 4);
	
	valid_mask = read64 (heap_tables + 8);
	sorted_mask = read64 (heap_tables + 16);
	rows = (const guint32 *) (heap_tables + 24);
	
	for (table = 0; table < 64; table++){
		if ((valid_mask & ((guint64) 1 << table)) == 0){
			if (table > MONO_TABLE_LAST)
				continue;
			image->tables [table].rows = 0;
			continue;
		}
		if (table > MONO_TABLE_LAST) {
			g_warning("bits in valid must be zero above 0x2d (II - 23.1.6)");
		} else {
			image->tables [table].rows = read32 (rows);
		}
		/*if ((sorted_mask & ((guint64) 1 << table)) == 0){
			g_print ("table %s (0x%02x) is sorted\n", mono_meta_table_name (table), table);
		}*/
		rows++;
		valid++;
	}

	image->tables_base = (heap_tables + 24) + (4 * valid);

	/* They must be the same */
	g_assert ((const void *) image->tables_base == (const void *) rows);

	mono_metadata_compute_table_bases (image);
	return TRUE;
}

static gboolean
load_metadata (MonoImage *image, MonoCLIImageInfo *iinfo)
{
	if (!load_metadata_ptrs (image, iinfo))
		return FALSE;

	return load_tables (image);
}

void
mono_image_check_for_module_cctor (MonoImage *image)
{
	MonoTableInfo *t, *mt;
	t = &image->tables [MONO_TABLE_TYPEDEF];
	mt = &image->tables [MONO_TABLE_METHOD];
	if (mono_get_runtime_info ()->framework_version [0] == '1') {
		image->checked_module_cctor = TRUE;
		return;
	}
	if (image->dynamic) {
		/* FIXME: */
		image->checked_module_cctor = TRUE;
		return;
	}
	if (t->rows >= 1) {
		guint32 nameidx = mono_metadata_decode_row_col (t, 0, MONO_TYPEDEF_NAME);
		const char *name = mono_metadata_string_heap (image, nameidx);
		if (strcmp (name, "<Module>") == 0) {
			guint32 first_method = mono_metadata_decode_row_col (t, 0, MONO_TYPEDEF_METHOD_LIST) - 1;
			guint32 last_method;
			if (t->rows > 1)
				last_method = mono_metadata_decode_row_col (t, 1, MONO_TYPEDEF_METHOD_LIST) - 1;
			else 
				last_method = mt->rows;
			for (; first_method < last_method; first_method++) {
				nameidx = mono_metadata_decode_row_col (mt, first_method, MONO_METHOD_NAME);
				name = mono_metadata_string_heap (image, nameidx);
				if (strcmp (name, ".cctor") == 0) {
					image->has_module_cctor = TRUE;
					image->checked_module_cctor = TRUE;
					return;
				}
			}
		}
	}
	image->has_module_cctor = FALSE;
	image->checked_module_cctor = TRUE;
}

static void
load_modules (MonoImage *image)
{
	MonoTableInfo *t;

	if (image->modules)
		return;

	t = &image->tables [MONO_TABLE_MODULEREF];
	image->modules = g_new0 (MonoImage *, t->rows);
	image->modules_loaded = g_new0 (gboolean, t->rows);
	image->module_count = t->rows;
}

/**
 * mono_image_load_module:
 *
 *   Load the module with the one-based index IDX from IMAGE and return it. Return NULL if
 * it cannot be loaded.
 */
MonoImage*
mono_image_load_module (MonoImage *image, int idx)
{
	MonoTableInfo *t;
	MonoTableInfo *file_table;
	int i;
	char *base_dir;
	gboolean refonly = image->ref_only;
	GList *list_iter, *valid_modules = NULL;
	MonoImageOpenStatus status;

	g_assert (idx <= image->module_count);
	if (image->modules_loaded [idx - 1])
		return image->modules [idx - 1];

	file_table = &image->tables [MONO_TABLE_FILE];
	for (i = 0; i < file_table->rows; i++) {
		guint32 cols [MONO_FILE_SIZE];
		mono_metadata_decode_row (file_table, i, cols, MONO_FILE_SIZE);
		if (cols [MONO_FILE_FLAGS] == FILE_CONTAINS_NO_METADATA)
			continue;
		valid_modules = g_list_prepend (valid_modules, (char*)mono_metadata_string_heap (image, cols [MONO_FILE_NAME]));
	}

	t = &image->tables [MONO_TABLE_MODULEREF];
	base_dir = g_path_get_dirname (image->name);

	{
		char *module_ref;
		const char *name;
		guint32 cols [MONO_MODULEREF_SIZE];
		/* if there is no file table, we try to load the module... */
		int valid = file_table->rows == 0;

		mono_metadata_decode_row (t, idx - 1, cols, MONO_MODULEREF_SIZE);
		name = mono_metadata_string_heap (image, cols [MONO_MODULEREF_NAME]);
		for (list_iter = valid_modules; list_iter; list_iter = list_iter->next) {
			/* be safe with string dups, but we could just compare string indexes  */
			if (strcmp (list_iter->data, name) == 0) {
				valid = TRUE;
				break;
			}
		}
		if (valid) {
			module_ref = g_build_filename (base_dir, name, NULL);
			image->modules [idx - 1] = mono_image_open_full (module_ref, &status, refonly);
			if (image->modules [idx - 1]) {
				mono_image_addref (image->modules [idx - 1]);
				image->modules [idx - 1]->assembly = image->assembly;
				/* g_print ("loaded module %s from %s (%p)\n", module_ref, image->name, image->assembly); */
			}
			g_free (module_ref);
		}
	}

	image->modules_loaded [idx - 1] = TRUE;

	g_free (base_dir);
	g_list_free (valid_modules);

	return image->modules [idx - 1];
}

static void
register_guid (gpointer key, gpointer value, gpointer user_data)
{
	MonoImage *image = (MonoImage*)value;

	if (!g_hash_table_lookup (loaded_images_guid_hash, image))
		g_hash_table_insert (loaded_images_guid_hash, image->guid, image);
}

static void
build_guid_table (gboolean refonly)
{
	GHashTable *loaded_images = refonly ? loaded_images_refonly_hash : loaded_images_hash;
	g_hash_table_foreach (loaded_images, register_guid, NULL);
}

static gpointer
class_key_extract (gpointer value)
{
	MonoClass *class = value;

	return GUINT_TO_POINTER (class->type_token);
}

static gpointer*
class_next_value (gpointer value)
{
	MonoClass *class = value;

	return (gpointer*)&class->next_class_cache;
}

void
mono_image_init (MonoImage *image)
{
	image->mempool = mono_mempool_new ();
	image->method_cache = g_hash_table_new (NULL, NULL);
	mono_internal_hash_table_init (&image->class_cache,
				       g_direct_hash,
				       class_key_extract,
				       class_next_value);
	image->field_cache = g_hash_table_new (NULL, NULL);

	image->delegate_begin_invoke_cache = 
		g_hash_table_new ((GHashFunc)mono_signature_hash, 
				  (GCompareFunc)mono_metadata_signature_equal);
	image->delegate_end_invoke_cache = 
		g_hash_table_new ((GHashFunc)mono_signature_hash, 
				  (GCompareFunc)mono_metadata_signature_equal);
	image->delegate_invoke_cache = 
		g_hash_table_new ((GHashFunc)mono_signature_hash, 
				  (GCompareFunc)mono_metadata_signature_equal);
	image->runtime_invoke_cache  = 
		g_hash_table_new ((GHashFunc)mono_signature_hash, 
				  (GCompareFunc)mono_metadata_signature_equal);
	
	image->managed_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->native_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->remoting_invoke_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->cominterop_invoke_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->cominterop_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->synchronized_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->unbox_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);

	image->ldfld_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->ldflda_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->ldfld_remote_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->stfld_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->stfld_remote_wrapper_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->isinst_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->castclass_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);
	image->proxy_isinst_cache = g_hash_table_new (mono_aligned_addr_hash, NULL);

	image->typespec_cache = g_hash_table_new (NULL, NULL);
	image->memberref_signatures = g_hash_table_new (NULL, NULL);
	image->helper_signatures = g_hash_table_new (g_str_hash, g_str_equal);
	image->method_signatures = g_hash_table_new (NULL, NULL);
}

static MonoImage *
do_mono_image_load (MonoImage *image, MonoImageOpenStatus *status,
		    gboolean care_about_cli)
{
	MonoCLIImageInfo *iinfo;
	MonoDotNetHeader *header;
	MonoMSDOSHeader msdos;
	guint32 offset = 0;

	mono_image_init (image);

	iinfo = image->image_info;
	header = &iinfo->cli_header;
		
	if (status)
		*status = MONO_IMAGE_IMAGE_INVALID;

	if (offset + sizeof (msdos) > image->raw_data_len)
		goto invalid_image;
	memcpy (&msdos, image->raw_data + offset, sizeof (msdos));
	
	if (!(msdos.msdos_sig [0] == 'M' && msdos.msdos_sig [1] == 'Z'))
		goto invalid_image;
	
	msdos.pe_offset = GUINT32_FROM_LE (msdos.pe_offset);

	offset = msdos.pe_offset;

	if (offset + sizeof (MonoDotNetHeader) > image->raw_data_len)
		goto invalid_image;
	memcpy (header, image->raw_data + offset, sizeof (MonoDotNetHeader));
	offset += sizeof (MonoDotNetHeader);

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#define SWAP32(x) (x) = GUINT32_FROM_LE ((x))
#define SWAP16(x) (x) = GUINT16_FROM_LE ((x))
#define SWAPPDE(x) do { (x).rva = GUINT32_FROM_LE ((x).rva); (x).size = GUINT32_FROM_LE ((x).size);} while (0)
	SWAP32 (header->coff.coff_time);
	SWAP32 (header->coff.coff_symptr);
	SWAP32 (header->coff.coff_symcount);
	SWAP16 (header->coff.coff_machine);
	SWAP16 (header->coff.coff_sections);
	SWAP16 (header->coff.coff_opt_header_size);
	SWAP16 (header->coff.coff_attributes);
	/* MonoPEHeader */
	SWAP32 (header->pe.pe_code_size);
	SWAP32 (header->pe.pe_data_size);
	SWAP32 (header->pe.pe_uninit_data_size);
	SWAP32 (header->pe.pe_rva_entry_point);
	SWAP32 (header->pe.pe_rva_code_base);
	SWAP32 (header->pe.pe_rva_data_base);
	SWAP16 (header->pe.pe_magic);

	/* MonoPEHeaderNT: not used yet */
	SWAP32	(header->nt.pe_image_base); 	/* must be 0x400000 */
	SWAP32	(header->nt.pe_section_align);       /* must be 8192 */
	SWAP32	(header->nt.pe_file_alignment);      /* must be 512 or 4096 */
	SWAP16	(header->nt.pe_os_major);            /* must be 4 */
	SWAP16	(header->nt.pe_os_minor);            /* must be 0 */
	SWAP16	(header->nt.pe_user_major);
	SWAP16	(header->nt.pe_user_minor);
	SWAP16	(header->nt.pe_subsys_major);
	SWAP16	(header->nt.pe_subsys_minor);
	SWAP32	(header->nt.pe_reserved_1);
	SWAP32	(header->nt.pe_image_size);
	SWAP32	(header->nt.pe_header_size);
	SWAP32	(header->nt.pe_checksum);
	SWAP16	(header->nt.pe_subsys_required);
	SWAP16	(header->nt.pe_dll_flags);
	SWAP32	(header->nt.pe_stack_reserve);
	SWAP32	(header->nt.pe_stack_commit);
	SWAP32	(header->nt.pe_heap_reserve);
	SWAP32	(header->nt.pe_heap_commit);
	SWAP32	(header->nt.pe_loader_flags);
	SWAP32	(header->nt.pe_data_dir_count);

	/* MonoDotNetHeader: mostly unused */
	SWAPPDE (header->datadir.pe_export_table);
	SWAPPDE (header->datadir.pe_import_table);
	SWAPPDE (header->datadir.pe_resource_table);
	SWAPPDE (header->datadir.pe_exception_table);
	SWAPPDE (header->datadir.pe_certificate_table);
	SWAPPDE (header->datadir.pe_reloc_table);
	SWAPPDE (header->datadir.pe_debug);
	SWAPPDE (header->datadir.pe_copyright);
	SWAPPDE (header->datadir.pe_global_ptr);
	SWAPPDE (header->datadir.pe_tls_table);
	SWAPPDE (header->datadir.pe_load_config_table);
	SWAPPDE (header->datadir.pe_bound_import);
	SWAPPDE (header->datadir.pe_iat);
	SWAPPDE (header->datadir.pe_delay_import_desc);
 	SWAPPDE (header->datadir.pe_cli_header);
	SWAPPDE (header->datadir.pe_reserved);

#undef SWAP32
#undef SWAP16
#undef SWAPPDE
#endif

	if (header->coff.coff_machine != 0x14c)
		goto invalid_image;

	if (header->coff.coff_opt_header_size != (sizeof (MonoDotNetHeader) - sizeof (MonoCOFFHeader) - 4))
		goto invalid_image;

	if (header->pesig[0] != 'P' || header->pesig[1] != 'E' || header->pe.pe_magic != 0x10B)
		goto invalid_image;

#if 0
	/*
	 * The spec says that this field should contain 6.0, but Visual Studio includes a new compiler,
	 * which produces binaries with 7.0.  From Sergey:
	 *
	 * The reason is that MSVC7 uses traditional compile/link
	 * sequence for CIL executables, and VS.NET (and Framework
	 * SDK) includes linker version 7, that puts 7.0 in this
	 * field.  That's why it's currently not possible to load VC
	 * binaries with Mono.  This field is pretty much meaningless
	 * anyway (what linker?).
	 */
	if (header->pe.pe_major != 6 || header->pe.pe_minor != 0)
		goto invalid_image;
#endif

	/*
	 * FIXME: byte swap all addresses here for header.
	 */
	
	if (!load_section_tables (image, iinfo, offset))
		goto invalid_image;
	
	if (care_about_cli == FALSE) {
		goto done;
	}
	
	/* Load the CLI header */
	if (!load_cli_header (image, iinfo))
		goto invalid_image;

	if (!load_metadata (image, iinfo))
		goto invalid_image;

	/* modules don't have an assembly table row */
	if (image->tables [MONO_TABLE_ASSEMBLY].rows)
		image->assembly_name = mono_metadata_string_heap (image, 
			mono_metadata_decode_row_col (&image->tables [MONO_TABLE_ASSEMBLY],
					0, MONO_ASSEMBLY_NAME));

	image->module_name = mono_metadata_string_heap (image, 
			mono_metadata_decode_row_col (&image->tables [MONO_TABLE_MODULE],
					0, MONO_MODULE_NAME));

	load_modules (image);

done:
	if (status)
		*status = MONO_IMAGE_OK;

	return image;

invalid_image:
	mono_image_close (image);
		return NULL;
}

static MonoImage *
do_mono_image_open (const char *fname, MonoImageOpenStatus *status,
		    gboolean care_about_cli, gboolean refonly)
{
	MonoCLIImageInfo *iinfo;
	MonoImage *image;
	FILE *filed;
	struct stat stat_buf;

	if ((filed = fopen (fname, "rb")) == NULL){
		if (status)
			*status = MONO_IMAGE_ERROR_ERRNO;
		return NULL;
	}

	if (fstat (fileno (filed), &stat_buf)) {
		fclose (filed);
		if (status)
			*status = MONO_IMAGE_ERROR_ERRNO;
		return NULL;
	}
	image = g_new0 (MonoImage, 1);
	image->file_descr = filed;
	image->raw_data_len = stat_buf.st_size;
	image->raw_data = mono_raw_buffer_load (fileno (filed), FALSE, 0, stat_buf.st_size);
	iinfo = g_new0 (MonoCLIImageInfo, 1);
	image->image_info = iinfo;
	image->name = mono_path_resolve_symlinks (fname);
	image->ref_only = refonly;
	image->ref_count = 1;

	return do_mono_image_load (image, status, care_about_cli);
}

MonoImage *
mono_image_loaded_full (const char *name, gboolean refonly)
{
	MonoImage *res;
	GHashTable *loaded_images = refonly ? loaded_images_refonly_hash : loaded_images_hash;
        
	mono_images_lock ();
	res = g_hash_table_lookup (loaded_images, name);
	mono_images_unlock ();
	return res;
}

/**
 * mono_image_loaded:
 * @name: name of the image to load
 *
 * This routine ensures that the given image is loaded.
 *
 * Returns: the loaded MonoImage, or NULL on failure.
 */
MonoImage *
mono_image_loaded (const char *name)
{
	return mono_image_loaded_full (name, FALSE);
}

MonoImage *
mono_image_loaded_by_guid_full (const char *guid, gboolean refonly)
{
	MonoImage *res;
	GHashTable *loaded_images = refonly ? loaded_images_refonly_guid_hash : loaded_images_guid_hash;

	mono_images_lock ();
	res = g_hash_table_lookup (loaded_images, guid);
	mono_images_unlock ();
	return res;
}

MonoImage *
mono_image_loaded_by_guid (const char *guid)
{
	return mono_image_loaded_by_guid_full (guid, FALSE);
}

static MonoImage *
register_image (MonoImage *image)
{
	MonoImage *image2;
	GHashTable *loaded_images = image->ref_only ? loaded_images_refonly_hash : loaded_images_hash;

	mono_images_lock ();
	image2 = g_hash_table_lookup (loaded_images, image->name);

	if (image2) {
		/* Somebody else beat us to it */
		mono_image_addref (image2);
		mono_images_unlock ();
		mono_image_close (image);
		return image2;
	}
	g_hash_table_insert (loaded_images, image->name, image);
	if (image->assembly_name && (g_hash_table_lookup (loaded_images, image->assembly_name) == NULL))
		g_hash_table_insert (loaded_images, (char *) image->assembly_name, image);	
	g_hash_table_insert (image->ref_only ? loaded_images_refonly_guid_hash : loaded_images_guid_hash, image->guid, image);
	mono_images_unlock ();

	return image;
}

MonoImage *
mono_image_open_from_data_full (char *data, guint32 data_len, gboolean need_copy, MonoImageOpenStatus *status, gboolean refonly)
{
	MonoCLIImageInfo *iinfo;
	MonoImage *image;
	char *datac;

	if (!data || !data_len) {
		if (status)
			*status = MONO_IMAGE_IMAGE_INVALID;
		return NULL;
	}
	datac = data;
	if (need_copy) {
		datac = g_try_malloc (data_len);
		if (!datac) {
			if (status)
				*status = MONO_IMAGE_ERROR_ERRNO;
			return NULL;
		}
		memcpy (datac, data, data_len);
	}

	image = g_new0 (MonoImage, 1);
	image->raw_data = datac;
	image->raw_data_len = data_len;
	image->raw_data_allocated = need_copy;
	image->name = g_strdup_printf ("data-%p", datac);
	iinfo = g_new0 (MonoCLIImageInfo, 1);
	image->image_info = iinfo;
	image->ref_only = refonly;

	image = do_mono_image_load (image, status, TRUE);
	if (image == NULL)
		return NULL;

	return register_image (image);
}

MonoImage *
mono_image_open_from_data (char *data, guint32 data_len, gboolean need_copy, MonoImageOpenStatus *status)
{
	return mono_image_open_from_data_full (data, data_len, need_copy, status, FALSE);
}

MonoImage *
mono_image_open_full (const char *fname, MonoImageOpenStatus *status, gboolean refonly)
{
	MonoImage *image;
	GHashTable *loaded_images;
	char *absfname;
	
	g_return_val_if_fail (fname != NULL, NULL);
	
	absfname = mono_path_canonicalize (fname);

	/*
	 * The easiest solution would be to do all the loading inside the mutex,
	 * but that would lead to scalability problems. So we let the loading
	 * happen outside the mutex, and if multiple threads happen to load
	 * the same image, we discard all but the first copy.
	 */
	mono_images_lock ();
	loaded_images = refonly ? loaded_images_refonly_hash : loaded_images_hash;
	image = g_hash_table_lookup (loaded_images, absfname);
	g_free (absfname);
	
	if (image){
		mono_image_addref (image);
		mono_images_unlock ();
		return image;
	}
	mono_images_unlock ();

	image = do_mono_image_open (fname, status, TRUE, refonly);
	if (image == NULL)
		return NULL;

	return register_image (image);
}

/**
 * mono_image_open:
 * @fname: filename that points to the module we want to open
 * @status: An error condition is returned in this field
 *
 * Returns: An open image of type %MonoImage or NULL on error. 
 * The caller holds a temporary reference to the returned image which should be cleared 
 * when no longer needed by calling mono_image_close ().
 * if NULL, then check the value of @status for details on the error
 */
MonoImage *
mono_image_open (const char *fname, MonoImageOpenStatus *status)
{
	return mono_image_open_full (fname, status, FALSE);
}

/**
 * mono_pe_file_open:
 * @fname: filename that points to the module we want to open
 * @status: An error condition is returned in this field
 *
 * Returns: An open image of type %MonoImage or NULL on error.  if
 * NULL, then check the value of @status for details on the error.
 * This variant for mono_image_open DOES NOT SET UP CLI METADATA.
 * It's just a PE file loader, used for FileVersionInfo.  It also does
 * not use the image cache.
 */
MonoImage *
mono_pe_file_open (const char *fname, MonoImageOpenStatus *status)
{
	g_return_val_if_fail (fname != NULL, NULL);
	
	return(do_mono_image_open (fname, status, FALSE, FALSE));
}

static void
free_hash_table (gpointer key, gpointer val, gpointer user_data)
{
	g_hash_table_destroy ((GHashTable*)val);
}

/*
static void
free_mr_signatures (gpointer key, gpointer val, gpointer user_data)
{
	mono_metadata_free_method_signature ((MonoMethodSignature*)val);
}
*/

static void
free_blob_cache_entry (gpointer key, gpointer val, gpointer user_data)
{
	g_free (key);
}

static void
free_remoting_wrappers (gpointer key, gpointer val, gpointer user_data)
{
	g_free (val);
}

static void
free_array_cache_entry (gpointer key, gpointer val, gpointer user_data)
{
	g_slist_free ((GSList*)val);
}

/**
 * mono_image_addref:
 * @image: The image file we wish to add a reference to
 *
 *  Increases the reference count of an image.
 */
void
mono_image_addref (MonoImage *image)
{
	InterlockedIncrement (&image->ref_count);
}	

void
mono_dynamic_stream_reset (MonoDynamicStream* stream)
{
	stream->alloc_size = stream->index = stream->offset = 0;
	g_free (stream->data);
	stream->data = NULL;
	if (stream->hash) {
		g_hash_table_destroy (stream->hash);
		stream->hash = NULL;
	}
}

/**
 * mono_image_close:
 * @image: The image file we wish to close
 *
 * Closes an image file, deallocates all memory consumed and
 * unmaps all possible sections of the file
 */
void
mono_image_close (MonoImage *image)
{
	MonoImage *image2;
	GHashTable *loaded_images, *loaded_images_guid;
	int i;

	g_return_if_fail (image != NULL);

	if (InterlockedDecrement (&image->ref_count) > 0)
		return;

	mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_ASSEMBLY, "Unloading image %s [%p].", image->name, image);

	mono_metadata_clean_for_image (image);

	mono_images_lock ();
	loaded_images = image->ref_only ? loaded_images_refonly_hash : loaded_images_hash;
	loaded_images_guid = image->ref_only ? loaded_images_refonly_guid_hash : loaded_images_guid_hash;
	image2 = g_hash_table_lookup (loaded_images, image->name);
	if (image == image2) {
		/* This is not true if we are called from mono_image_open () */
		g_hash_table_remove (loaded_images, image->name);
		g_hash_table_remove (loaded_images_guid, image->guid);
	}
	if (image->assembly_name && (g_hash_table_lookup (loaded_images, image->assembly_name) == image))
		g_hash_table_remove (loaded_images, (char *) image->assembly_name);	

	/* Multiple images might have the same guid */
	build_guid_table (image->ref_only);

	mono_images_unlock ();

	if (image->file_descr) {
		fclose (image->file_descr);
		image->file_descr = NULL;
		if (image->raw_data != NULL)
			mono_raw_buffer_free (image->raw_data);
	}
	
	if (image->raw_data_allocated) {
		/* image->raw_metadata and cli_sections might lie inside image->raw_data */
		MonoCLIImageInfo *ii = image->image_info;

		if ((image->raw_metadata > image->raw_data) &&
			(image->raw_metadata <= (image->raw_data + image->raw_data_len)))
			image->raw_metadata = NULL;

		for (i = 0; i < ii->cli_section_count; i++)
			if (((char*)(ii->cli_sections [i]) > image->raw_data) &&
				((char*)(ii->cli_sections [i]) <= ((char*)image->raw_data + image->raw_data_len)))
				ii->cli_sections [i] = NULL;

		g_free (image->raw_data);
	}

	if (debug_assembly_unload) {
		image->name = g_strdup_printf ("%s - UNLOADED", image->name);
	} else {
		g_free (image->name);
		g_free (image->guid);
		g_free (image->version);
		g_free (image->files);
	}

	g_hash_table_destroy (image->method_cache);
	mono_internal_hash_table_destroy (&image->class_cache);
	g_hash_table_destroy (image->field_cache);
	if (image->array_cache) {
		g_hash_table_foreach (image->array_cache, free_array_cache_entry, NULL);
		g_hash_table_destroy (image->array_cache);
	}
	if (image->ptr_cache)
		g_hash_table_destroy (image->ptr_cache);
	if (image->name_cache) {
		g_hash_table_foreach (image->name_cache, free_hash_table, NULL);
		g_hash_table_destroy (image->name_cache);
	}
	g_hash_table_destroy (image->native_wrapper_cache);
	g_hash_table_destroy (image->managed_wrapper_cache);
	g_hash_table_destroy (image->delegate_begin_invoke_cache);
	g_hash_table_destroy (image->delegate_end_invoke_cache);
	g_hash_table_destroy (image->delegate_invoke_cache);
	g_hash_table_foreach (image->remoting_invoke_cache, free_remoting_wrappers, NULL);
	g_hash_table_destroy (image->remoting_invoke_cache);
	g_hash_table_destroy (image->runtime_invoke_cache);
	g_hash_table_destroy (image->synchronized_cache);
	g_hash_table_destroy (image->unbox_wrapper_cache);
	g_hash_table_destroy (image->cominterop_invoke_cache);
	g_hash_table_destroy (image->cominterop_wrapper_cache);
	g_hash_table_destroy (image->typespec_cache);
	g_hash_table_destroy (image->ldfld_wrapper_cache);
	g_hash_table_destroy (image->ldflda_wrapper_cache);
	g_hash_table_destroy (image->ldfld_remote_wrapper_cache);
	g_hash_table_destroy (image->stfld_wrapper_cache);
	g_hash_table_destroy (image->stfld_remote_wrapper_cache);
	g_hash_table_destroy (image->isinst_cache);
	g_hash_table_destroy (image->castclass_cache);
	g_hash_table_destroy (image->proxy_isinst_cache);

	/* The ownership of signatures is not well defined */
	//g_hash_table_foreach (image->memberref_signatures, free_mr_signatures, NULL);
	g_hash_table_destroy (image->memberref_signatures);
	//g_hash_table_foreach (image->helper_signatures, free_mr_signatures, NULL);
	g_hash_table_destroy (image->helper_signatures);
	g_hash_table_destroy (image->method_signatures);

	if (image->interface_bitset) {
		mono_unload_interface_ids (image->interface_bitset);
		mono_bitset_free (image->interface_bitset);
	}
	if (image->image_info){
		MonoCLIImageInfo *ii = image->image_info;

		if (ii->cli_section_tables)
			g_free (ii->cli_section_tables);
		if (ii->cli_sections)
			g_free (ii->cli_sections);
		g_free (image->image_info);
	}

	for (i = 0; i < image->module_count; ++i) {
		if (image->modules [i])
			mono_image_close (image->modules [i]);
	}
	if (image->modules)
		g_free (image->modules);
	if (image->modules_loaded)
		g_free (image->modules_loaded);
	if (image->references)
		g_free (image->references);
	/*g_print ("destroy image %p (dynamic: %d)\n", image, image->dynamic);*/
	if (!image->dynamic) {
		if (debug_assembly_unload)
			mono_mempool_invalidate (image->mempool);
		else {
			mono_mempool_destroy (image->mempool);
			g_free (image);
		}
	} else {
		/* Dynamic images are GC_MALLOCed */
		struct _MonoDynamicImage *di = (struct _MonoDynamicImage*)image;
		int i;
		g_free ((char*)image->module_name);
		if (di->typespec)
			g_hash_table_destroy (di->typespec);
		if (di->typeref)
			g_hash_table_destroy (di->typeref);
		if (di->handleref)
			g_hash_table_destroy (di->handleref);
		if (di->tokens)
			mono_g_hash_table_destroy (di->tokens);
		if (di->blob_cache) {
			g_hash_table_foreach (di->blob_cache, free_blob_cache_entry, NULL);
			g_hash_table_destroy (di->blob_cache);
		}
		g_list_free (di->array_methods);
		if (di->gen_params)
			g_ptr_array_free (di->gen_params, TRUE);
		if (di->token_fixups)
			mono_g_hash_table_destroy (di->token_fixups);
		if (di->method_to_table_idx)
			g_hash_table_destroy (di->method_to_table_idx);
		if (di->field_to_table_idx)
			g_hash_table_destroy (di->field_to_table_idx);
		if (di->method_aux_hash)
			g_hash_table_destroy (di->method_aux_hash);
		g_free (di->strong_name);
		g_free (di->win32_res);
		/*g_print ("string heap destroy for image %p\n", di);*/
		mono_dynamic_stream_reset (&di->sheap);
		mono_dynamic_stream_reset (&di->code);
		mono_dynamic_stream_reset (&di->resources);
		mono_dynamic_stream_reset (&di->us);
		mono_dynamic_stream_reset (&di->blob);
		mono_dynamic_stream_reset (&di->tstream);
		mono_dynamic_stream_reset (&di->guid);
		for (i = 0; i < MONO_TABLE_NUM; ++i) {
			g_free (di->tables [i].values);
		}
		mono_mempool_destroy (image->mempool);
	}
}

/** 
 * mono_image_strerror:
 * @status: an code indicating the result from a recent operation
 *
 * Returns: a string describing the error
 */
const char *
mono_image_strerror (MonoImageOpenStatus status)
{
	switch (status){
	case MONO_IMAGE_OK:
		return "success";
	case MONO_IMAGE_ERROR_ERRNO:
		return strerror (errno);
	case MONO_IMAGE_IMAGE_INVALID:
		return "File does not contain a valid CIL image";
	case MONO_IMAGE_MISSING_ASSEMBLYREF:
		return "An assembly was referenced, but could not be found";
	}
	return "Internal error";
}

static gpointer
mono_image_walk_resource_tree (MonoCLIImageInfo *info, guint32 res_id,
			       guint32 lang_id, gunichar2 *name,
			       MonoPEResourceDirEntry *entry,
			       MonoPEResourceDir *root, guint32 level)
{
	gboolean is_string, is_dir;
	guint32 name_offset, dir_offset;

	/* Level 0 holds a directory entry for each type of resource
	 * (identified by ID or name).
	 *
	 * Level 1 holds a directory entry for each named resource
	 * item, and each "anonymous" item of a particular type of
	 * resource.
	 *
	 * Level 2 holds a directory entry for each language pointing to
	 * the actual data.
	 */
	name_offset = GUINT32_FROM_LE (entry->name_offset) & 0x7fffffff;
	dir_offset = GUINT32_FROM_LE (entry->dir_offset) & 0x7fffffff;

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	is_string = (GUINT32_FROM_LE (entry->name_offset) & 0x80000000) != 0;
	is_dir = (GUINT32_FROM_LE (entry->dir_offset) & 0x80000000) != 0;
#else
	is_string = entry->name_is_string;
	is_dir = entry->is_dir;
#endif

	if(level==0) {
		if((is_string==FALSE && name_offset!=res_id) ||
		   (is_string==TRUE)) {
			return(NULL);
		}
	} else if (level==1) {
#if 0
		if(name!=NULL &&
		   is_string==TRUE && name!=lookup (name_offset)) {
			return(NULL);
		}
#endif
	} else if (level==2) {
		if ((is_string == FALSE &&
		    name_offset != lang_id &&
		    lang_id != 0) ||
		   (is_string == TRUE)) {
			return(NULL);
		}
	} else {
		g_assert_not_reached ();
	}

	if(is_dir==TRUE) {
		MonoPEResourceDir *res_dir=(MonoPEResourceDir *)(((char *)root)+dir_offset);
		MonoPEResourceDirEntry *sub_entries=(MonoPEResourceDirEntry *)(res_dir+1);
		guint32 entries, i;

		entries = GUINT16_FROM_LE (res_dir->res_named_entries) + GUINT16_FROM_LE (res_dir->res_id_entries);

		for(i=0; i<entries; i++) {
			MonoPEResourceDirEntry *sub_entry=&sub_entries[i];
			gpointer ret;
			
			ret=mono_image_walk_resource_tree (info, res_id,
							   lang_id, name,
							   sub_entry, root,
							   level+1);
			if(ret!=NULL) {
				return(ret);
			}
		}

		return(NULL);
	} else {
		MonoPEResourceDataEntry *data_entry=(MonoPEResourceDataEntry *)((char *)(root)+dir_offset);
		MonoPEResourceDataEntry *res;

		res = g_new0 (MonoPEResourceDataEntry, 1);

		res->rde_data_offset = GUINT32_TO_LE (data_entry->rde_data_offset);
		res->rde_size = GUINT32_TO_LE (data_entry->rde_size);
		res->rde_codepage = GUINT32_TO_LE (data_entry->rde_codepage);
		res->rde_reserved = GUINT32_TO_LE (data_entry->rde_reserved);

		return (res);
	}
}

/**
 * mono_image_lookup_resource:
 * @image: the image to look up the resource in
 * @res_id: A MONO_PE_RESOURCE_ID_ that represents the resource ID to lookup.
 * @lang_id: The language id.
 * @name: the resource name to lookup.
 *
 * Returns: NULL if not found, otherwise a pointer to the in-memory representation
 * of the given resource. The caller should free it using g_free () when no longer
 * needed.
 */
gpointer
mono_image_lookup_resource (MonoImage *image, guint32 res_id, guint32 lang_id, gunichar2 *name)
{
	MonoCLIImageInfo *info;
	MonoDotNetHeader *header;
	MonoPEDatadir *datadir;
	MonoPEDirEntry *rsrc;
	MonoPEResourceDir *resource_dir;
	MonoPEResourceDirEntry *res_entries;
	guint32 entries, i;

	if(image==NULL) {
		return(NULL);
	}

	info=image->image_info;
	if(info==NULL) {
		return(NULL);
	}

	header=&info->cli_header;
	if(header==NULL) {
		return(NULL);
	}

	datadir=&header->datadir;
	if(datadir==NULL) {
		return(NULL);
	}

	rsrc=&datadir->pe_resource_table;
	if(rsrc==NULL) {
		return(NULL);
	}

	resource_dir=(MonoPEResourceDir *)mono_image_rva_map (image, rsrc->rva);
	if(resource_dir==NULL) {
		return(NULL);
	}

	entries = GUINT16_FROM_LE (resource_dir->res_named_entries) + GUINT16_FROM_LE (resource_dir->res_id_entries);
	res_entries=(MonoPEResourceDirEntry *)(resource_dir+1);
	
	for(i=0; i<entries; i++) {
		MonoPEResourceDirEntry *entry=&res_entries[i];
		gpointer ret;
		
		ret=mono_image_walk_resource_tree (info, res_id, lang_id,
						   name, entry, resource_dir,
						   0);
		if(ret!=NULL) {
			return(ret);
		}
	}

	return(NULL);
}

/** 
 * mono_image_get_entry_point:
 * @image: the image where the entry point will be looked up.
 *
 * Use this routine to determine the metadata token for method that
 * has been flagged as the entry point.
 *
 * Returns: the token for the entry point method in the image
 */
guint32
mono_image_get_entry_point (MonoImage *image)
{
	return ((MonoCLIImageInfo*)image->image_info)->cli_cli_header.ch_entry_point;
}

/**
 * mono_image_get_resource:
 * @image: the image where the resource will be looked up.
 * @offset: The offset to add to the resource
 * @size: a pointer to an int where the size of the resource will be stored
 *
 * This is a low-level routine that fetches a resource from the
 * metadata that starts at a given @offset.  The @size parameter is
 * filled with the data field as encoded in the metadata.
 *
 * Returns: the pointer to the resource whose offset is @offset.
 */
const char*
mono_image_get_resource (MonoImage *image, guint32 offset, guint32 *size)
{
	MonoCLIImageInfo *iinfo = image->image_info;
	MonoCLIHeader *ch = &iinfo->cli_cli_header;
	const char* data;

	if (!ch->ch_resources.rva || offset + 4 > ch->ch_resources.size)
		return NULL;
	
	data = mono_image_rva_map (image, ch->ch_resources.rva);
	if (!data)
		return NULL;
	data += offset;
	if (size)
		*size = read32 (data);
	data += 4;
	return data;
}

MonoImage*
mono_image_load_file_for_image (MonoImage *image, int fileidx)
{
	char *base_dir, *name;
	MonoImage *res;
	MonoTableInfo  *t = &image->tables [MONO_TABLE_FILE];
	const char *fname;
	guint32 fname_id;

	if (fileidx < 1 || fileidx > t->rows)
		return NULL;

	if (image->files && image->files [fileidx - 1])
		return image->files [fileidx - 1];

	if (!image->files)
		image->files = g_new0 (MonoImage*, t->rows);

	fname_id = mono_metadata_decode_row_col (t, fileidx - 1, MONO_FILE_NAME);
	fname = mono_metadata_string_heap (image, fname_id);
	base_dir = g_path_get_dirname (image->name);
	name = g_build_filename (base_dir, fname, NULL);
	res = mono_image_open (name, NULL);
	if (res) {
		int i;
		/* g_print ("loaded file %s from %s (%p)\n", name, image->name, image->assembly); */
		res->assembly = image->assembly;
		for (i = 0; i < res->module_count; ++i) {
			if (res->modules [i] && !res->modules [i]->assembly)
				res->modules [i]->assembly = image->assembly;
		}

		image->files [fileidx - 1] = res;
	}
	g_free (name);
	g_free (base_dir);
	return res;
}

/**
 * mono_image_get_strong_name:
 * @image: a MonoImage
 * @size: a guint32 pointer, or NULL.
 *
 * If the image has a strong name, and @size is not NULL, the value
 * pointed to by size will have the size of the strong name.
 *
 * Returns: NULL if the image does not have a strong name, or a
 * pointer to the public key.
 */
const char*
mono_image_get_strong_name (MonoImage *image, guint32 *size)
{
	MonoCLIImageInfo *iinfo = image->image_info;
	MonoPEDirEntry *de = &iinfo->cli_cli_header.ch_strong_name;
	const char* data;

	if (!de->size || !de->rva)
		return NULL;
	data = mono_image_rva_map (image, de->rva);
	if (!data)
		return NULL;
	if (size)
		*size = de->size;
	return data;
}

/**
 * mono_image_strong_name_position:
 * @image: a MonoImage
 * @size: a guint32 pointer, or NULL.
 *
 * If the image has a strong name, and @size is not NULL, the value
 * pointed to by size will have the size of the strong name.
 *
 * Returns: the position within the image file where the strong name
 * is stored.
 */
guint32
mono_image_strong_name_position (MonoImage *image, guint32 *size)
{
	MonoCLIImageInfo *iinfo = image->image_info;
	MonoPEDirEntry *de = &iinfo->cli_cli_header.ch_strong_name;
	const int top = iinfo->cli_section_count;
	MonoSectionTable *tables = iinfo->cli_section_tables;
	int i;
	guint32 addr = de->rva;
	
	if (size)
		*size = de->size;
	if (!de->size || !de->rva)
		return 0;
	for (i = 0; i < top; i++){
		if ((addr >= tables->st_virtual_address) &&
		    (addr < tables->st_virtual_address + tables->st_raw_data_size)){
			return tables->st_raw_data_ptr +
				(addr - tables->st_virtual_address);
		}
		tables++;
	}

	return 0;
}

/**
 * mono_image_get_public_key:
 * @image: a MonoImage
 * @size: a guint32 pointer, or NULL.
 *
 * This is used to obtain the public key in the @image.
 * 
 * If the image has a public key, and @size is not NULL, the value
 * pointed to by size will have the size of the public key.
 * 
 * Returns: NULL if the image does not have a public key, or a pointer
 * to the public key.
 */
const char*
mono_image_get_public_key (MonoImage *image, guint32 *size)
{
	const char *pubkey;
	guint32 len, tok;
	if (image->tables [MONO_TABLE_ASSEMBLY].rows != 1)
		return NULL;
	tok = mono_metadata_decode_row_col (&image->tables [MONO_TABLE_ASSEMBLY], 0, MONO_ASSEMBLY_PUBLIC_KEY);
	if (!tok)
		return NULL;
	pubkey = mono_metadata_blob_heap (image, tok);
	len = mono_metadata_decode_blob_size (pubkey, &pubkey);
	if (size)
		*size = len;
	return pubkey;
}

/**
 * mono_image_get_name:
 * @name: a MonoImage
 *
 * Returns: the name of the assembly.
 */
const char*
mono_image_get_name (MonoImage *image)
{
	return image->assembly_name;
}

/**
 * mono_image_get_filename:
 * @image: a MonoImage
 *
 * Used to get the filename that hold the actual MonoImage
 *
 * Returns: the filename.
 */
const char*
mono_image_get_filename (MonoImage *image)
{
	return image->name;
}

const char*
mono_image_get_guid (MonoImage *image)
{
	return image->guid;
}

const MonoTableInfo*
mono_image_get_table_info (MonoImage *image, int table_id)
{
	if (table_id < 0 || table_id >= MONO_TABLE_NUM)
		return NULL;
	return &image->tables [table_id];
}

int
mono_image_get_table_rows (MonoImage *image, int table_id)
{
	if (table_id < 0 || table_id >= MONO_TABLE_NUM)
		return 0;
	return image->tables [table_id].rows;
}

int
mono_table_info_get_rows (const MonoTableInfo *table)
{
	return table->rows;
}

/**
 * mono_image_get_assembly:
 * @image: the MonoImage.
 *
 * Use this routine to get the assembly that owns this image.
 *
 * Returns: the assembly that holds this image.
 */
MonoAssembly* 
mono_image_get_assembly (MonoImage *image)
{
	return image->assembly;
}

/**
 * mono_image_is_dynamic:
 * @image: the MonoImage
 *
 * Determines if the given image was created dynamically through the
 * System.Reflection.Emit API
 *
 * Returns: TRUE if the image was created dynamically, FALSE if not.
 */
gboolean
mono_image_is_dynamic (MonoImage *image)
{
	return image->dynamic;
}

/**
 * mono_image_has_authenticode_entry:
 * @image: the MonoImage
 *
 * Use this routine to determine if the image has a Authenticode
 * Certificate Table.
 *
 * Returns: TRUE if the image contains an authenticode entry in the PE
 * directory.
 */
gboolean
mono_image_has_authenticode_entry (MonoImage *image)
{
	MonoCLIImageInfo *iinfo = image->image_info;
	MonoDotNetHeader *header = &iinfo->cli_header;
	MonoPEDirEntry *de = &header->datadir.pe_certificate_table;
	// the Authenticode "pre" (non ASN.1) header is 8 bytes long
	return ((de->rva != 0) && (de->size > 8));
}
