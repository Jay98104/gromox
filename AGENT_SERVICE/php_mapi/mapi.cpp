#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <cstdint>
#include "php.h"
#include "php_mapi.h"
#include "ext_pack.h"
#include "zarafa_client.h"
#include "type_conversion.h"
#include <memory>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "ext/standard/info.h"
#include "Zend/zend_exceptions.h"
#include <sys/wait.h>
#include "ext.hpp"

#define TOOLS_PATH									"/var/pandora/tools"

#define PR_ATTACH_DATA_OBJ                          0x3701000D
#define PR_CONTENTS_SYNCHRONIZER					0x662D000D
#define PR_HIERARCHY_SYNCHRONIZER					0x662C000D
#define PR_COLLECTOR								0x662E000D

#define ZEND_FETCH_RESOURCE(rsrc, rsrc_type, passed_id, default_id, resource_type_name, resource_type) \
	do { \
		rsrc = static_cast<rsrc_type>(zend_fetch_resource(Z_RES_P(*passed_id), resource_type_name, resource_type)); \
		if (rsrc == nullptr) do { \
			RETURN_FALSE; \
		} while (false); \
	} while (false)
#define ZEND_REGISTER_RESOURCE(return_value, ses, le_mapi_thing) \
	do { \
		ZVAL_RES(return_value, zend_register_resource(ses, le_mapi_thing)); \
	} while (false)

typedef struct _MAPI_RESOURCE {
	uint8_t type;
	GUID hsession;
	uint32_t hobject;
} MAPI_RESOURCE;

typedef struct _STREAM_OBJECT {
	GUID hsession;
	uint32_t hparent;
	uint32_t proptag;
	uint32_t seek_offset;
	BINARY content_bin;
} STREAM_OBJECT;

typedef struct _ICS_IMPORT_CTX {
	GUID hsession;
	uint32_t hobject;
	uint8_t ics_type;
	zval pztarget_obj;
#ifdef ZTS
	TSRMLS_D;
#endif
} ICS_IMPORT_CTX;

typedef struct _ICS_EXPORT_CTX {
	GUID hsession;
	uint32_t hobject;
	uint8_t ics_type;
	zval pztarget_obj;
#ifdef ZTS
	TSRMLS_D;
#endif
	zend_bool b_changed;
	uint32_t progress;
	uint32_t sync_steps;
	uint32_t total_steps;
} ICS_EXPORT_CTX;

/* Not defined anymore in PHP 5.3.0 */
#if ZEND_MODULE_API_NO >= 20071006
ZEND_BEGIN_ARG_INFO(first_arg_force_ref, 0)
	ZEND_ARG_PASS_INFO(1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(second_arg_force_ref, 0)
	ZEND_ARG_PASS_INFO(0)
	ZEND_ARG_PASS_INFO(1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(fourth_arg_force_ref, 0)
	ZEND_ARG_PASS_INFO(0)
	ZEND_ARG_PASS_INFO(0)
	ZEND_ARG_PASS_INFO(0)
	ZEND_ARG_PASS_INFO(1)
ZEND_END_ARG_INFO()
#endif

static zend_function_entry mapi_functions[] = {
    ZEND_FE(mapi_last_hresult, NULL)
	ZEND_FE(mapi_prop_type, NULL)
	ZEND_FE(mapi_prop_id, NULL)
	ZEND_FE(mapi_is_error, NULL)
	ZEND_FE(mapi_make_scode, NULL)
	ZEND_FE(mapi_prop_tag, NULL)
	ZEND_FE(mapi_createoneoff, NULL)
	ZEND_FE(mapi_parseoneoff, NULL)
	ZEND_FE(mapi_logon_zarafa, NULL)
	ZEND_FE(mapi_logon_ex, NULL)
	ZEND_FE(mapi_getmsgstorestable, NULL)
	ZEND_FE(mapi_openmsgstore, NULL)
	ZEND_FE(mapi_openprofilesection, NULL)

	ZEND_FE(mapi_openaddressbook, NULL)
	ZEND_FE(mapi_openentry, NULL)
	ZEND_FE(mapi_ab_openentry, NULL)
	ZEND_FE(mapi_ab_resolvename, NULL)
	ZEND_FE(mapi_ab_getdefaultdir, NULL)

	ZEND_FE(mapi_msgstore_createentryid, NULL)
	ZEND_FE(mapi_msgstore_getarchiveentryid, NULL)
	ZEND_FE(mapi_msgstore_openentry, NULL)
	ZEND_FE(mapi_msgstore_getreceivefolder, NULL)
	ZEND_FE(mapi_msgstore_entryidfromsourcekey, NULL)
	ZEND_FE(mapi_msgstore_advise, NULL)
	ZEND_FE(mapi_msgstore_unadvise, NULL)
	ZEND_FE(mapi_msgstore_abortsubmit, NULL)
	
	ZEND_FE(mapi_sink_create, NULL)
	ZEND_FE(mapi_sink_timedwait, NULL)

	ZEND_FE(mapi_table_queryallrows, NULL)
	ZEND_FE(mapi_table_queryrows, NULL)
	ZEND_FE(mapi_table_getrowcount, NULL)
	ZEND_FE(mapi_table_setcolumns, NULL)
	ZEND_FE(mapi_table_seekrow, NULL)
	ZEND_FE(mapi_table_sort, NULL)
	ZEND_FE(mapi_table_restrict, NULL)
	ZEND_FE(mapi_table_findrow, NULL)
	ZEND_FE(mapi_table_createbookmark, NULL)
	ZEND_FE(mapi_table_freebookmark, NULL)

	ZEND_FE(mapi_folder_gethierarchytable, NULL)
	ZEND_FE(mapi_folder_getcontentstable, NULL)
	ZEND_FE(mapi_folder_getrulestable, NULL)
	ZEND_FE(mapi_folder_createmessage, NULL)
	ZEND_FE(mapi_folder_createfolder, NULL)
	ZEND_FE(mapi_folder_deletemessages, NULL)
	ZEND_FE(mapi_folder_copymessages, NULL)
	ZEND_FE(mapi_folder_emptyfolder, NULL)
	ZEND_FE(mapi_folder_copyfolder, NULL)
	ZEND_FE(mapi_folder_deletefolder, NULL)
	ZEND_FE(mapi_folder_setreadflags, NULL)
	ZEND_FE(mapi_folder_setsearchcriteria, NULL)
	ZEND_FE(mapi_folder_getsearchcriteria, NULL)
	ZEND_FE(mapi_folder_modifyrules, NULL)

	ZEND_FE(mapi_message_getattachmenttable, NULL)
	ZEND_FE(mapi_message_getrecipienttable, NULL)
	ZEND_FE(mapi_message_openattach, NULL)
	ZEND_FE(mapi_message_createattach, NULL)
	ZEND_FE(mapi_message_deleteattach, NULL)
	ZEND_FE(mapi_message_modifyrecipients, NULL)
	ZEND_FE(mapi_message_submitmessage, NULL)
	ZEND_FE(mapi_message_setreadflag, NULL)

	ZEND_FE(mapi_openpropertytostream, NULL)
	ZEND_FE(mapi_stream_write, NULL)
	ZEND_FE(mapi_stream_read, NULL)
	ZEND_FE(mapi_stream_stat, NULL)
	ZEND_FE(mapi_stream_seek, NULL)
	ZEND_FE(mapi_stream_commit, NULL)
	ZEND_FE(mapi_stream_setsize, NULL)
	ZEND_FE(mapi_stream_create, NULL)

	ZEND_FE(mapi_attach_openobj, NULL)
	ZEND_FE(mapi_savechanges, NULL)
	ZEND_FE(mapi_getprops, NULL)
	ZEND_FE(mapi_setprops, NULL)
	ZEND_FE(mapi_copyto, NULL)
	ZEND_FE(mapi_openproperty, NULL)
	ZEND_FE(mapi_deleteprops, NULL)
	ZEND_FE(mapi_getnamesfromids, NULL)
	ZEND_FE(mapi_getidsfromnames, NULL)
	ZEND_FE(mapi_decompressrtf, NULL)
	
	ZEND_FE(mapi_zarafa_getpermissionrules, NULL)
	ZEND_FE(mapi_zarafa_setpermissionrules, NULL)
	
	ZEND_FE(mapi_getuseravailability, NULL)

	ZEND_FE(mapi_exportchanges_config, NULL)
	ZEND_FE(mapi_exportchanges_synchronize, NULL)
	ZEND_FE(mapi_exportchanges_updatestate, NULL)
	ZEND_FE(mapi_exportchanges_getchangecount, NULL)
	ZEND_FE(mapi_importcontentschanges_config, NULL)
	ZEND_FE(mapi_importcontentschanges_updatestate, NULL)
	ZEND_FE(mapi_importcontentschanges_importmessagechange,
		fourth_arg_force_ref)
	ZEND_FE(mapi_importcontentschanges_importmessagedeletion, NULL)
	ZEND_FE(mapi_importcontentschanges_importperuserreadstatechange, NULL)
	ZEND_FE(mapi_importcontentschanges_importmessagemove, NULL)
	ZEND_FE(mapi_importhierarchychanges_config, NULL)
	ZEND_FE(mapi_importhierarchychanges_updatestate, NULL)
	ZEND_FE(mapi_importhierarchychanges_importfolderchange, NULL)
	ZEND_FE(mapi_importhierarchychanges_importfolderdeletion, NULL)

	ZEND_FE(mapi_wrap_importcontentschanges, first_arg_force_ref)
	ZEND_FE(mapi_wrap_importhierarchychanges, first_arg_force_ref)

	ZEND_FE(mapi_inetmapi_imtoinet, NULL)
	ZEND_FE(mapi_inetmapi_imtomapi, NULL)
	ZEND_FE(mapi_icaltomapi, NULL)
	ZEND_FE(mapi_mapitoical, NULL)
	ZEND_FE(mapi_vcftomapi, NULL)
	ZEND_FE(mapi_mapitovcf, NULL)

	ZEND_FE(mapi_enable_exceptions, NULL)
	
    ZEND_FE(mapi_feature, NULL)

	ZEND_FALIAS(mapi_attach_openbin, mapi_openproperty, NULL)
	ZEND_FALIAS(mapi_msgstore_getprops, mapi_getprops, NULL)
	ZEND_FALIAS(mapi_folder_getprops, mapi_getprops, NULL)
	ZEND_FALIAS(mapi_message_getprops, mapi_getprops, NULL)
	ZEND_FALIAS(mapi_message_setprops, mapi_setprops, NULL)
	ZEND_FALIAS(mapi_message_openproperty, mapi_openproperty, NULL)
	ZEND_FALIAS(mapi_attach_getprops, mapi_getprops, NULL)
	ZEND_FALIAS(mapi_attach_openproperty, mapi_openproperty, NULL)
	ZEND_FALIAS(mapi_message_savechanges, mapi_savechanges, NULL)

	ZEND_FE(kc_session_save, second_arg_force_ref)
	ZEND_FE(kc_session_restore, second_arg_force_ref)
	
	ZEND_FE(nsp_getuserinfo, NULL)
	ZEND_FE(nsp_setuserpasswd, NULL)
	
	ZEND_FE(mapi_linkmessage, NULL)
	
	{NULL, NULL, NULL}
};

ZEND_DECLARE_MODULE_GLOBALS(mapi)

static void php_mapi_init_globals(zend_mapi_globals *mapi_globals)
{
	/* nothing to do */
}

zend_module_entry mapi_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_MAPI_EXTNAME,
    mapi_functions,
    PHP_MINIT(mapi),
    PHP_MSHUTDOWN(mapi),
    PHP_RINIT(mapi),
	PHP_RSHUTDOWN(mapi),
    PHP_MINFO(mapi),
    PHP_MAPI_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_MAPI
ZEND_GET_MODULE(mapi)
#endif

static char name_mapi_session[] = "MAPI Session";
static char name_mapi_table[] = "MAPI Table";
static char name_mapi_msgstore[] = "MAPI Message Store";
static char name_mapi_addressbook[] = "MAPI Addressbook";
static char name_mapi_mailuser[] = "MAPI Mail User";
static char name_mapi_distlist[] = "MAPI Distribution List";
static char name_mapi_abcont[] = "MAPI Addressbook Container";
static char name_mapi_folder[] = "MAPI Folder";
static char name_mapi_message[] = "MAPI Message";
static char name_mapi_attachment[] = "MAPI Attachment";
static char name_mapi_property[] = "MAPI Property";
static char name_stream[] = "IStream Interface";
static char name_fb_data[] = "Freebusy Data Interface";
static char name_fb_update[] = "Freebusy Update Interface";
static char name_fb_enumblock[] = "Freebusy Enumblock Interface";
static char name_mapi_exportchanges[] = "ICS Export Changes";
static char name_mapi_advisesink[] = "MAPI Advise sink";
static char name_mapi_importhierarchychanges[] =
					"ICS Import Hierarchy Changes";
static const char name_mapi_importcontentschanges[] =
						"ICS Import Contents Changes";

static int le_stream;
static int le_mapi_table;
static int le_mapi_abcont;
static int le_mapi_folder;
static int le_mapi_message;
static int le_mapi_session;
static int le_mapi_addressbook;
static int le_mapi_msgstore;
static int le_mapi_mailuser;
static int le_mapi_distlist;
static int le_mapi_property;
static int le_mapi_advisesink;
static int le_mapi_attachment;
static int le_mapi_exportchanges;
static int le_mapi_importcontentschanges;
static int le_mapi_importhierarchychanges;

static GUID GUID_NONE = {0x00000000, 0x0000, 0x0000,
	{0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
static GUID IID_IStream = {0x0000000c, 0x0000, 0x0000,
	{0xc0, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static GUID IID_IMessage = {0x00020307, 0x0000, 0x0000,
	{0xc0, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static GUID IID_IExchangeExportChanges = {0xa3ea9cc0, 0xd1b2,
	0x11cd, {0x80, 0xfc}, {0x00, 0xaa, 0x00, 0x4b, 0xba, 0x0b}};
static GUID IID_IExchangeImportContentsChanges = {0xf75abfa0,
	0xd0e0, 0x11cd, {0x80, 0xfc}, {0x00, 0xaa, 0x00, 0x4b, 0xba, 0x0b}};
static GUID IID_IExchangeImportHierarchyChanges = {0x85a66cf0,
	0xd0e0, 0x11cd, {0x80, 0xfc}, {0x00, 0xaa, 0x00, 0x4b, 0xba, 0x0b}};

static STREAM_OBJECT* stream_object_create()
{
	auto pstream = st_calloc<STREAM_OBJECT>();
	if (NULL == pstream) {
		return NULL;
	}
	return pstream;
}

static zend_bool stream_object_set_length(
	STREAM_OBJECT *pstream, uint32_t length)
{
	uint8_t *pdata;
	
	/* always leave trail null for string */
	if (NULL == pstream->content_bin.pb) {
		pstream->content_bin.pb = sta_malloc<uint8_t>(length + 1);
		if (NULL == pstream->content_bin.pb) {
			return 0;
		}
		memset(pstream->content_bin.pb, 0, length + 1);
	} else if (length > pstream->content_bin.cb) {
		pdata = sta_realloc<uint8_t>(pstream->content_bin.pb, length + 1);
		if (NULL == pdata) {
			return 0;
		}
		pstream->content_bin.pb = pdata;
		memset(pstream->content_bin.pb
			+ pstream->content_bin.cb, 0,
			length + 1 - pstream->content_bin.cb);
	} else {
		if (pstream->seek_offset > length) {
			pstream->seek_offset = length;
		}
		pstream->content_bin.pb[length] = '\0';
	}
	pstream->content_bin.cb = length;
	return 1;
}

static void* stream_object_read(STREAM_OBJECT *pstream,
	uint32_t buf_len, uint32_t *pactual_bytes)
{
	void *paddress;
	uint32_t length;
	
	if (NULL == pstream->content_bin.pb ||
		pstream->content_bin.cb <= pstream->seek_offset) {
		*pactual_bytes = 0;
		return NULL;
	}
	if (pstream->seek_offset + buf_len > pstream->content_bin.cb) {
		length = pstream->content_bin.cb - pstream->seek_offset;
	} else {
		length = buf_len;
	}
	paddress = pstream->content_bin.pb + pstream->seek_offset;
	pstream->seek_offset += length;
	*pactual_bytes = length;
	return paddress;
}

static uint32_t stream_object_write(STREAM_OBJECT *pstream,
	void *pbuff, uint32_t buf_len)
{	
	if (NULL == pstream->content_bin.pb) {
		pstream->content_bin.pb = sta_malloc<uint8_t>(buf_len);
		if (NULL == pstream->content_bin.pb) {
			return 0;
		}
		pstream->seek_offset = 0;
	}
	if (pstream->seek_offset + buf_len > pstream->content_bin.cb) {
		if (!stream_object_set_length(pstream,
			pstream->seek_offset + buf_len)) {
			return 0;	
		}
	}
	memcpy(pstream->content_bin.pb +
		pstream->seek_offset, pbuff, buf_len);
	pstream->seek_offset += buf_len;
	return buf_len;
}

static void stream_object_set_parent(STREAM_OBJECT *pstream,
	GUID hsession, uint32_t hparent, uint32_t proptag)
{
	pstream->hsession = hsession;
	pstream->hparent = hparent;
	pstream->proptag = proptag;
}

static uint32_t stream_object_get_length(STREAM_OBJECT *pstream)
{
	return pstream->content_bin.cb;
}

static zend_bool stream_object_seek(STREAM_OBJECT *pstream,
	uint32_t flags, int32_t offset)
{	
	switch (flags) {
	case STREAM_SEEK_SET:
		if (offset <= 0) {
			pstream->seek_offset = 0;
			return 1;
		}
		if (offset > pstream->content_bin.cb) {
			if (!stream_object_set_length(pstream, offset)) {
				return 0;
			}
		}
		pstream->seek_offset = offset;
		return 1; 
	case STREAM_SEEK_CUR:
		if (offset < 0) {
			if (pstream->seek_offset + offset < 0) {
				pstream->seek_offset = 0;
				return 1;
			}
			pstream->seek_offset += offset;
			return 1;
		}
		if (offset + pstream->seek_offset
			> pstream->content_bin.cb) {
			if (!stream_object_set_length(pstream,
				pstream->seek_offset + offset)) {
				return 0;
			}
		}
		pstream->seek_offset += offset;
		return 1;
	case STREAM_SEEK_END:
		if (offset > 0) {
			if (!stream_object_set_length(pstream,
				pstream->content_bin.cb + offset)) {
				return 0;
			}
		}
		pstream->seek_offset = pstream->content_bin.cb + offset;
		if (pstream->seek_offset < 0) {
			pstream->seek_offset = 0;
		}
		return 1;
	}
	return 0;
}

static void stream_object_reset(STREAM_OBJECT *pstream)
{
	if (NULL != pstream->content_bin.pb) {
		efree(pstream->content_bin.pb);
	}
	memset(pstream, 0, sizeof(STREAM_OBJECT));
}

static void stream_object_free(STREAM_OBJECT *pstream)
{
	if (NULL != pstream->content_bin.pb) {
		efree(pstream->content_bin.pb);
	}
	efree(pstream);
}

static uint32_t stream_object_commit(STREAM_OBJECT *pstream)
{
	char *pstring;
	uint32_t result;
	
	if (0 == memcmp(&pstream->hsession, &GUID_NONE, sizeof(GUID))
		|| 0 == pstream->hparent || 0 == pstream->proptag) {
		return EC_INVALID_PARAMETER;
	}
	switch (pstream->proptag & 0xFFFF) {
	case PROPVAL_TYPE_BINARY:
		return zarafa_client_setpropval(
			pstream->hsession, pstream->hparent,
			pstream->proptag, &pstream->content_bin);
	case PROPVAL_TYPE_STRING:
	case PROPVAL_TYPE_WSTRING:
		return zarafa_client_setpropval(pstream->hsession,
			pstream->hparent, pstream->proptag & 0xFFFF0000
			| PROPVAL_TYPE_WSTRING, pstream->content_bin.pb);
	default:
		return EC_INVALID_PARAMETER;
	}
}

static BINARY* stream_object_get_content(STREAM_OBJECT *pstream)
{
	return &pstream->content_bin;
}

static NOTIF_SINK* notif_sink_create()
{
	auto psink = st_calloc<NOTIF_SINK>();
	if (NULL == psink) {
		return NULL;
	}
	return psink;
}

static void notif_sink_free(NOTIF_SINK *psink)
{
	int i;
	
	if (NULL != psink->padvise) {
		if (0 != memcmp(&GUID_NONE, &psink->hsession, sizeof(GUID))) {
			for (i=0; i<psink->count; i++) {
				zarafa_client_unadvise(psink->hsession,
					psink->padvise[i].hstore, psink->padvise[i].sub_id);
			}
		}
		efree(psink->padvise);
	}
	efree(psink);
}

static uint32_t notif_sink_timedwait(NOTIF_SINK *psink,
	uint32_t timeval, ZNOTIFICATION_ARRAY *pnotifications)
{
	if (0 == psink->count) {
		pnotifications->count = 0;
		pnotifications->ppnotification = NULL;
		return EC_SUCCESS;
	}
	return zarafa_client_notifdequeue(
		psink, timeval, pnotifications);
}

static zend_bool notif_sink_add_subscription(NOTIF_SINK *psink,
	GUID hsession, uint32_t hstore, uint32_t sub_id)
{
	ADVISE_INFO *padvise;
	
	if (NULL == psink->padvise) {
		padvise = st_malloc<ADVISE_INFO>();
	} else {
		padvise = sta_realloc<ADVISE_INFO>(psink->padvise, psink->count + 1);
	}
	if (NULL == padvise) {
		return 0;
	}
	if (0 == memcmp(&GUID_NONE, &psink->hsession, sizeof(GUID))) {
		psink->hsession = hsession;
	} else {
		if (0 != memcmp(&hsession, &psink->hsession, sizeof(GUID))) {
			return 0;
		}
	}
	padvise[psink->count].hstore = hstore;
	padvise[psink->count].sub_id = sub_id;
	psink->count ++;
	psink->padvise = padvise;
	return 1;
}

static void mapi_resource_dtor(zend_resource *rsrc TSRMLS_DC)
{
	MAPI_RESOURCE *presource;
	
	if (NULL == rsrc->ptr) {
		return;
	}
	presource = (MAPI_RESOURCE*)rsrc->ptr;
	if (0 != presource->hobject) {
		zarafa_client_unloadobject(
			presource->hsession, presource->hobject);
	}
	efree(presource);
}

static void notif_sink_dtor(zend_resource *rsrc TSRMLS_DC)
{
	if (NULL != rsrc->ptr) {
		notif_sink_free(static_cast<NOTIF_SINK *>(rsrc->ptr));
	}
}

static void stream_object_dtor(zend_resource *rsrc TSRMLS_DC)
{
	if (NULL != rsrc->ptr) {
		stream_object_free(static_cast<STREAM_OBJECT *>(rsrc->ptr));
	}
}

static void ics_import_ctx_dtor(zend_resource *rsrc TSRMLS_DC)
{	
	ICS_IMPORT_CTX *pctx;
	
	if (NULL == rsrc->ptr) {
		return;
	}
	pctx = (ICS_IMPORT_CTX*)rsrc->ptr;
	zval_ptr_dtor(&pctx->pztarget_obj);
	if (0 != pctx->hobject) {
		zarafa_client_unloadobject(
			pctx->hsession, pctx->hobject);
	}
	efree(pctx);
}

static void ics_export_ctx_dtor(zend_resource *rsrc TSRMLS_DC)
{
	ICS_EXPORT_CTX *pctx;
	
	if (NULL == rsrc->ptr) {
		return;
	}
	pctx = (ICS_EXPORT_CTX*)rsrc->ptr;
	zval_ptr_dtor(&pctx->pztarget_obj);
	if (0 != pctx->hobject) {
		zarafa_client_unloadobject(
			pctx->hsession, pctx->hobject);
	}
	efree(pctx);
}

PHP_MINIT_FUNCTION(mapi)
{
	le_mapi_session = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_session, module_number);
	le_mapi_addressbook = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_addressbook, module_number);
	le_mapi_table = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_table, module_number);
	le_mapi_msgstore = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_msgstore, module_number);
	le_mapi_mailuser = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_mailuser, module_number);
	le_mapi_distlist = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_distlist, module_number);
	le_mapi_abcont = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_abcont, module_number);
	le_mapi_folder = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_folder, module_number);
	le_mapi_message = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_message, module_number);
	le_mapi_attachment = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_attachment, module_number);
	le_mapi_property = zend_register_list_destructors_ex(
		mapi_resource_dtor, NULL, name_mapi_property, module_number);
	le_mapi_advisesink = zend_register_list_destructors_ex(
		notif_sink_dtor, NULL, name_mapi_advisesink, module_number);
	le_stream = zend_register_list_destructors_ex(
		stream_object_dtor, NULL, name_stream, module_number);
	le_mapi_exportchanges = zend_register_list_destructors_ex(
		ics_export_ctx_dtor, NULL, name_mapi_exportchanges,
		module_number);
	le_mapi_importhierarchychanges = zend_register_list_destructors_ex(
		ics_import_ctx_dtor, NULL, name_mapi_importhierarchychanges,
		module_number);
	le_mapi_importcontentschanges = zend_register_list_destructors_ex(
		ics_import_ctx_dtor, NULL, name_mapi_importcontentschanges,
		module_number);
	ZEND_INIT_MODULE_GLOBALS(mapi, php_mapi_init_globals, NULL);
	return SUCCESS;
}

PHP_MINFO_FUNCTION(mapi)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "MAPI Support", "enabled");
	php_info_print_table_row(2, "Version", "1.0");
	php_info_print_table_end();
}

PHP_MSHUTDOWN_FUNCTION(mapi)
{
	return SUCCESS;
}

PHP_RINIT_FUNCTION(mapi)
{
	zstrplus str_server(zend_string_init(ZEND_STRL("_SERVER"), 0));
	zstrplus str_user(zend_string_init(ZEND_STRL("REMOTE_USER"), 0));

	MAPI_G(hr) = 0;
	MAPI_G(exception_ce) = NULL;
	MAPI_G(exceptions_enabled) = 0;
	auto server_vars = zend_hash_find(&EG(symbol_table), str_server.get());
	if (server_vars == nullptr || Z_TYPE_P(server_vars) != IS_ARRAY)
		return SUCCESS;
	auto username = zend_hash_find(Z_ARRVAL_P(server_vars), str_user.get());
	if (username == nullptr || Z_TYPE_P(username) != IS_STRING ||
	    Z_STRLEN_P(username) == 0)
		return SUCCESS;
	add_assoc_stringl(server_vars, "PHP_AUTH_USER", Z_STRVAL_P(username), Z_STRLEN_P(username));
	add_assoc_string(server_vars, "PHP_AUTH_PW", "password");
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(mapi)
{
	return SUCCESS;
}

/*---------------------------------------------------------------------------*/

ZEND_FUNCTION(mapi_last_hresult)
{
	RETURN_LONG(MAPI_G(hr));
}

ZEND_FUNCTION(mapi_prop_type)
{
	long proptag;

	if (zend_parse_parameters(ZEND_NUM_ARGS()
		TSRMLS_CC, "l", &proptag) == FAILURE) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		RETVAL_FALSE;
	} else {
		MAPI_G(hr) = EC_SUCCESS;
		RETURN_LONG(proptag & 0xFFFF);
	}
}

ZEND_FUNCTION(mapi_prop_id)
{
	long proptag;

	if (zend_parse_parameters(ZEND_NUM_ARGS()
		TSRMLS_CC, "l", &proptag) == FAILURE) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		RETVAL_FALSE;
	} else {
		MAPI_G(hr) = EC_SUCCESS;
		RETURN_LONG(((uint32_t)proptag) >> 16);
	}
}

ZEND_FUNCTION(mapi_is_error)
{
	long errcode;

	if (zend_parse_parameters(ZEND_NUM_ARGS()
		TSRMLS_CC, "l", &errcode) == FAILURE) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		RETVAL_FALSE;
	} else {
		MAPI_G(hr) = EC_SUCCESS;
		RETURN_BOOL(errcode & 0x80000000);
	}
}

ZEND_FUNCTION(mapi_make_scode)
{
	long sev, code;
	uint32_t scode;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS()
		TSRMLS_CC, "ll", &sev, &code) == FAILURE) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		RETVAL_FALSE;
	} else {
		if (0 == sev) {
			scode = 0x40000 | ((uint32_t)code);
		} else {
			scode = 0x80000000 | 0x40000 | ((uint32_t)code);
		}
		MAPI_G(hr) = EC_SUCCESS;
		RETURN_LONG(scode);
	}
}

ZEND_FUNCTION(mapi_prop_tag)
{
	long propid;
	long proptype;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ll", &proptype, &propid) == FAILURE || propid >
		0xFFFF || proptype > 0xFFFF) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		RETVAL_FALSE;
	} else {
		MAPI_G(hr) = EC_SUCCESS;
		RETURN_LONG((propid << 16) | proptype);
	}
}

ZEND_FUNCTION(mapi_createoneoff)
{
	long flags;
	char *ptype;
	int type_len;
	int name_len;
	char *paddress;
	int address_len;
	PUSH_CTX push_ctx;
	char *pdisplayname;
	ONEOFF_ENTRYID tmp_entry;
	static const uint8_t oneoff_guid[] ={
		0x81, 0x2B, 0x1F, 0xA4, 0xBE, 0xA3, 0x10, 0x19,
		0x9D, 0x6E, 0x00, 0xDD, 0x01, 0x0F, 0x54, 0x02};
	char empty[1]{};
	
	flags = 0;
	name_len = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"sss|l", &pdisplayname, &name_len, &ptype, &type_len,
		&paddress, &address_len, &flags) == FAILURE ||
		NULL == ptype || '\0' == ptype[0] || NULL == paddress) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	if (NULL == pdisplayname) {
		pdisplayname = empty;
	}
	tmp_entry.flags = 0;
	memcpy(tmp_entry.provider_uid, oneoff_guid, 16);
	tmp_entry.version = 0;
	tmp_entry.ctrl_flags = flags;
	tmp_entry.pdisplay_name = pdisplayname;
	tmp_entry.paddress_type = ptype;
	tmp_entry.pmail_address = paddress;
	ext_pack_push_init(&push_ctx);
	if (!ext_pack_push_oneoff_entryid(&push_ctx, &tmp_entry)) {
		MAPI_G(hr) = EC_ERROR;
		ext_pack_push_free(&push_ctx);
		goto THROW_EXCEPTION;
	}
	RETVAL_STRINGL(reinterpret_cast<const char *>(push_ctx.data), push_ctx.offset);
	ext_pack_push_free(&push_ctx);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_parseoneoff)
{
	int cbentryid;
	char *pentryid;
	PULL_CTX pull_ctx;
	ONEOFF_ENTRYID oneoff_entry;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s",
		&pentryid, &cbentryid) == FAILURE || NULL == pentryid) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ext_pack_pull_init(&pull_ctx, reinterpret_cast<const uint8_t *>(pentryid), cbentryid);
	if (!ext_pack_pull_oneoff_entryid(&pull_ctx, &oneoff_entry)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	array_init(return_value);
	add_assoc_string(return_value, "name", oneoff_entry.pdisplay_name);
	add_assoc_string(return_value, "type", oneoff_entry.paddress_type);
	add_assoc_string(return_value, "address", oneoff_entry.pmail_address);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_logon_zarafa)
{
	long flags;
	int wa_len;
	int misc_len;
	char *server;
	char *sslcert;
	char *sslpass;
	char *username;
	char *password;
	int server_len;
	int sslcert_len;
	int sslpass_len;
	uint32_t result;
	int username_len;
	int password_len;
	char *wa_version;
	char *misc_version;
	MAPI_RESOURCE *presource;
	zstrplus str_server(zend_string_init(ZEND_STRL("_SERVER"), 0));
	zstrplus str_user(zend_string_init(ZEND_STRL("REMOTE_USER"), 0));
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|ssslss",
		&username, &username_len, &password, &password_len, &server,
		&server_len, &sslcert, &sslcert_len, &sslpass, &sslpass_len,
		&flags, &wa_version, &wa_len, &misc_version, &misc_len)
		== FAILURE || NULL == username || '\0' == username[0] ||
		NULL == password) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	{
	auto server_vars = zend_hash_find(&EG(symbol_table), str_server.get());
	if (server_vars != nullptr && Z_TYPE_P(server_vars) == IS_ARRAY) {
		auto username = zend_hash_find(Z_ARRVAL_P(server_vars), str_user.get());
		if (username != nullptr && Z_TYPE_P(username) == IS_STRING &&
		    Z_STRLEN_P(username) > 0)
			password = nullptr;
	}
	result = zarafa_client_logon(username, password, 0, &presource->hsession);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	}
	presource->type = MAPI_SESSION;
	presource->hobject = 0;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_session);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_logon_ex)
{
	long flags;
	char *username;
	char *password;
	uint32_t result;
	int username_len;
	int password_len;
	zval **ppzmethod;
	zval **ppzserver_vars;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ssl",
		&username, &username_len, &password, &password_len, &flags)
		== FAILURE || NULL == username || '\0' == username[0] ||
		NULL == password) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	if ('\0' == password[0]) {
		/* enable empty password only when php is running under cli mode */
		zstrplus str_server(zend_string_init(ZEND_STRL("_SERVER"), 0));
		zstrplus str_reqm(zend_string_init(ZEND_STRL("REQUEST_METHOD"), 0));

		if (PG(auto_globals_jit)) {
			zend_is_auto_global(str_server.get());
		}

		auto server_vars = zend_hash_find(&EG(symbol_table), str_server.get());
		if (server_vars != nullptr && Z_TYPE_P(server_vars) == IS_ARRAY) {
			auto method = zend_hash_find(Z_ARRVAL_P(server_vars), str_reqm.get());
			if (method != nullptr && Z_TYPE_P(method) == IS_STRING &&
			    Z_STRLEN_P(method) > 0) {
				MAPI_G(hr) = EC_ACCESS_DENIED;
				goto THROW_EXCEPTION;
			}
		}
		password = NULL;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_logon(username,
		password, flags, &presource->hsession);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_SESSION;
	presource->hobject = 0;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_session);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_openentry)
{
	long flags;
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	uint32_t hobject;
	uint8_t mapi_type;
	MAPI_RESOURCE *psession;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|sl",
		&pzresource, &entryid.pb, &entryid.cb, &flags) == FAILURE
		|| NULL == pzresource || NULL == entryid.pb) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openentry(psession->hsession,
					entryid, flags, &mapi_type, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = mapi_type;
	presource->hsession = psession->hsession;
	presource->hobject = hobject;
	switch (mapi_type) {
	case MAPI_FOLDER:
		ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_folder);
		break;
	case MAPI_MESSAGE:
		ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_message);
		break;
	default:
		efree(presource);
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_openaddressbook)
{
	zval *pzresource;
	MAPI_RESOURCE *psession;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_ADDRESSBOOK;
	presource->hsession = psession->hsession;
	presource->hobject = 0;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_addressbook);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_ab_openentry)
{
	long flags;
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	uint32_t hobject;
	uint8_t mapi_type;
	MAPI_RESOURCE *psession;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	entryid.cb = 0;
	entryid.pb = NULL;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|sl",
		&pzresource, &entryid.pb, &entryid.cb, &flags) == FAILURE
		|| NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_addressbook,
		le_mapi_addressbook);
	if (MAPI_ADDRESSBOOK != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openabentry(psession->hsession,
							entryid, &mapi_type, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = mapi_type;
	presource->hsession = psession->hsession;
	presource->hobject = hobject;
	switch (mapi_type) {
	case MAPI_MAILUSER:
		ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_mailuser);
		break;
	case MAPI_DISTLIST:
		ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_distlist);
		break;
	case MAPI_ABCONT:
		ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_abcont);
		break;
	default:
		efree(presource);
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_ab_resolvename)
{
	long flags;
	zval *pzarray;
	zval pzrowset;
	uint32_t result;
	zval *pzresource;
	TARRAY_SET cond_set;
	TARRAY_SET result_set;
	MAPI_RESOURCE *psession;
	
	ZVAL_NULL(&pzrowset);
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra|l", &pzresource, &pzarray, &flags) == FAILURE
		|| NULL == pzresource || NULL == pzarray) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_addressbook,
		le_mapi_addressbook);
	if (MAPI_ADDRESSBOOK != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_tarray_set(pzarray, &cond_set TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_resolvename(
		psession->hsession, &cond_set,
		&result_set);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (!tarray_set_to_php(&result_set, &pzrowset TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_ZVAL(&pzrowset, 0, 0);
	zval_ptr_dtor(&pzrowset);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
 	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_ab_getdefaultdir)
{
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *psession;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_addressbook,
		le_mapi_addressbook);
	if (MAPI_ADDRESSBOOK != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getabgal(psession->hsession, &entryid);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_STRINGL(reinterpret_cast<const char *>(entryid.pb), entryid.cb);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_getmsgstorestable)
{
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *psession;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_loadstoretable(psession->hsession, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_TABLE;
	presource->hsession = psession->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_table);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_openmsgstore)
{
	BINARY entryid;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *psession;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rs", &pzresource, &entryid.pb, &entryid.cb) ==
		FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openstore(
		psession->hsession, entryid,
		&hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_STORE;
	presource->hsession = psession->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_msgstore);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_openprofilesection)
{
	int uidlen;
	FLATUID *puid;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *psession;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rs", &pzresource, &puid, &uidlen) == FAILURE ||
		NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (NULL != puid) {
		if (0 == uidlen) {
			puid = NULL;
		} else if (uidlen != sizeof(FLATUID)) {
			MAPI_G(hr) = EC_INVALID_PARAMETER;
			goto THROW_EXCEPTION;
		}
	}
	result = zarafa_client_openpropfilesec(
		psession->hsession, puid, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_PROFPROPERTY;
	presource->hsession = psession->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_property);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_gethierarchytable)
{
	int type;
	long flags;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *probject;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_abcont) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_abcont, le_mapi_abcont);
		if (MAPI_ABCONT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	result = zarafa_client_loadhierarchytable(
		probject->hsession, probject->hobject,
		flags, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_TABLE;
	presource->hsession = probject->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_table);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_getcontentstable)
{
	int type;
	long flags;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *probject;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_abcont) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_abcont, le_mapi_abcont);
		if (MAPI_ABCONT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	result = zarafa_client_loadcontenttable(
		probject->hsession, probject->hobject,
		flags, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_TABLE;
	presource->hsession = probject->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_table);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_createmessage)
{
	long flags;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_createmessage(
		pfolder->hsession, pfolder->hobject,
		flags, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_MESSAGE;
	presource->hsession = pfolder->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_message);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_deletemessages)
{
	long flags;
	zval *pzarray;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	BINARY_ARRAY entryid_array;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra|l", &pzresource, &pzarray, &flags) == FAILURE
		|| NULL == pzresource || NULL == pzarray) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_binary_array(pzarray, &entryid_array TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_deletemessages(
		pfolder->hsession, pfolder->hobject,
		&entryid_array, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_copymessages)
{
	long flags;
	zval *pzarray;
	uint32_t result;
	zval *pzsrcfolder;
	zval *pzdstfolder;
	MAPI_RESOURCE *psrcfolder;
	MAPI_RESOURCE *pdstfolder;
	BINARY_ARRAY entryid_array;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rar|l",
		&pzsrcfolder, &pzarray, &pzdstfolder, &flags) == FAILURE ||
		NULL == pzsrcfolder || NULL == pzarray || NULL == pzdstfolder) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psrcfolder, MAPI_RESOURCE*,
		&pzsrcfolder, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != psrcfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pdstfolder, MAPI_RESOURCE*,
		&pzdstfolder, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pdstfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_binary_array(pzarray, &entryid_array TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_copymessages(
		psrcfolder->hsession, psrcfolder->hobject,
		pdstfolder->hobject, &entryid_array, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_setreadflags)
{
	long flags;
	zval *pzarray;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	BINARY_ARRAY entryid_array;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra|l", &pzresource, &pzarray, &flags) == FAILURE
		|| NULL == pzresource || NULL == pzarray) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_binary_array(pzarray, &entryid_array TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_setreadflags(
		pfolder->hsession, pfolder->hobject,
		&entryid_array, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_createfolder)
{
	int flags;
	char *pfname;
	int name_len;
	char *pcomment;
	int comment_len;
	uint32_t result;
	long folder_type;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	MAPI_RESOURCE *presource;
	char empty[1]{};
	
	flags = 0;
	pcomment = NULL;
	comment_len = 0;
	folder_type = FOLDER_TYPE_GENERIC;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|sll",
		&pzresource, &pfname, &name_len, &pcomment, &comment_len,
		&flags, &folder_type) == FAILURE || NULL == pzresource ||
		NULL == pfname || '\0' == pfname[0]) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	if (NULL == pcomment || 0 == comment_len) {
		pcomment = empty;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_createfolder(
		pfolder->hsession, pfolder->hobject,
		folder_type, pfname, pcomment, flags,
		&hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_FOLDER;
	presource->hsession = pfolder->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_folder);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_deletefolder)
{
	long flags;
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|l",
		&pzresource, &entryid.pb, &entryid.cb, &flags) == FAILURE
		|| NULL == pzresource || NULL == entryid.pb) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_deletefolder(
		pfolder->hsession, pfolder->hobject,
		entryid, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_emptyfolder)
{
	long flags;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_emptyfolder(
		pfolder->hsession, pfolder->hobject,
		flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_copyfolder)
{
	long flags;
	char *pname;
	int name_len;
	BINARY entryid;
	uint32_t result;
	zval *pzvalsrcfolder;
	zval *pzvaldstfolder;
	MAPI_RESOURCE *psrcfolder;
	MAPI_RESOURCE *pdstfolder;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rsr|sl",
		&pzvalsrcfolder, &entryid.pb, &entryid.cb, &pzvaldstfolder,
		&pname, &name_len, &flags) == FAILURE || NULL == pzvalsrcfolder ||
		NULL == entryid.pb || 0 == entryid.cb || NULL == pzvaldstfolder) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psrcfolder, MAPI_RESOURCE*,
		&pzvalsrcfolder, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != psrcfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pdstfolder, MAPI_RESOURCE*,
		&pzvaldstfolder, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pdstfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (0 == name_len) {
		pname = NULL;
	}
	result = zarafa_client_copyfolder(psrcfolder->hsession,
		psrcfolder->hobject, entryid, pdstfolder->hobject,
		pname, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_createentryid)
{
	int dn_len;
	BINARY entryid;
	char *mailboxdn;
	uint32_t result;
	zval *pzresource;
	
	if (!zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rs", &pzresource, &mailboxdn, &dn_len) == FAILURE
		|| NULL == mailboxdn || '\0' == mailboxdn[0]) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getstoreentryid(mailboxdn, &entryid);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_STRINGL(reinterpret_cast<const char *>(entryid.pb), entryid.cb);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_getarchiveentryid)
{
	MAPI_G(hr) = EC_NOT_FOUND;
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_openentry)
{
	long flags;
	BINARY entryid;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	uint8_t mapi_type;
	MAPI_RESOURCE *pstore;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	entryid.cb = 0;
	entryid.pb = NULL;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r|sl", &pzresource, &entryid.pb, &entryid.cb,
		&flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openstoreentry(pstore->hsession,
		pstore->hobject, entryid, flags, &mapi_type, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = mapi_type;
	presource->hsession = pstore->hsession;
	presource->hobject = hobject;
	if (MAPI_FOLDER == mapi_type) {
		ZEND_REGISTER_RESOURCE(return_value,
			presource, le_mapi_folder);
	} else if (MAPI_MESSAGE == mapi_type) {
		ZEND_REGISTER_RESOURCE(return_value,
			presource, le_mapi_message);
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_entryidfromsourcekey)
{
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	BINARY *pmessage_key;
	MAPI_RESOURCE *pstore;
	BINARY sourcekey_folder;
	BINARY sourcekey_message;
	
	sourcekey_message.cb = 0;
	sourcekey_message.pb = NULL;
	if (!zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|s",
		&pzresource, &sourcekey_folder.pb, &sourcekey_folder.cb,
		&sourcekey_message.pb, &sourcekey_message.cb) == FAILURE
		|| NULL == pzresource || NULL == sourcekey_folder.pb ||
		0 == sourcekey_folder.cb) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (NULL == sourcekey_message.pb || 0 == sourcekey_message.cb) {
		pmessage_key = NULL;
	} else {
		pmessage_key = &sourcekey_message;
	}
	result = zarafa_client_entryidfromsourcekey(pstore->hsession,
		pstore->hobject, sourcekey_folder, pmessage_key, &entryid);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_STRINGL(reinterpret_cast<const char *>(entryid.pb), entryid.cb);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_advise)
{
	BINARY entryid;
	uint32_t result;
	uint32_t sub_id;
	long event_mask;
	zval *pzressink;
	zval *pzresource;
	BINARY *pentryid;
	NOTIF_SINK *psink;
	MAPI_RESOURCE *pstore;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rslr", &pzresource, &entryid.pb, &entryid.cb,
		&event_mask, &pzressink) == FAILURE || NULL ==
		pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psink, NOTIF_SINK*, &pzressink,
		-1, name_mapi_advisesink, le_mapi_advisesink);
	if (NULL == entryid.pb || 0 == entryid.cb) {
		pentryid = NULL;
	} else {
		pentryid = &entryid;
	}
	result = zarafa_client_storeadvise(
		pstore->hsession, pstore->hobject,
		pentryid, event_mask, &sub_id);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (!notif_sink_add_subscription(psink,
		pstore->hsession, pstore->hobject, sub_id)) {
		zarafa_client_unadvise(pstore->hsession,
			pstore->hobject, sub_id);
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	RETVAL_LONG(sub_id);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_unadvise)
{
	long sub_id;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pstore;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl",
		&pzresource, &sub_id) == FAILURE || NULL == pzresource
		|| 0 == sub_id) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_unadvise(pstore->hsession,
							pstore->hobject, sub_id);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_sink_create)
{
	NOTIF_SINK *psink;
    
	psink = notif_sink_create();
	if (NULL == psink) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		RETVAL_FALSE;
		if (MAPI_G(exceptions_enabled)) {
			zend_throw_exception(MAPI_G(exception_ce),
				"MAPI error ", MAPI_G(hr) TSRMLS_CC);
		}
	} else {
		MAPI_G(hr) = EC_SUCCESS;
		ZEND_REGISTER_RESOURCE(return_value, psink, le_mapi_advisesink);
	}	
}

ZEND_FUNCTION(mapi_sink_timedwait)
{
	long tmp_time;
	uint32_t result;
	zval *pzressink;
	NOTIF_SINK *psink;
	zval pznotifications;
	ZNOTIFICATION_ARRAY notifications;

	ZVAL_NULL(&pznotifications);
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl",
		&pzressink, &tmp_time) == FAILURE || NULL == pzressink) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto RETURN_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psink, NOTIF_SINK*, &pzressink,
		-1, name_mapi_advisesink, le_mapi_advisesink);
	if (0 == psink->count) {
		usleep(tmp_time*1000);
		notifications.count = 0;
		notifications.ppnotification = NULL;
	} else {
		tmp_time /= 1000;
		if (tmp_time < 1) {
			tmp_time = 1;
		}
		result = notif_sink_timedwait(psink,
				tmp_time, &notifications);
		if (EC_SUCCESS != result) {
			MAPI_G(hr) = result;
			goto RETURN_EXCEPTION;
		}
	}
	if (!znotification_array_to_php(&notifications,
		&pznotifications TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto RETURN_EXCEPTION;
	}
	RETVAL_ZVAL(&pznotifications, 0, 0);
	zval_ptr_dtor(&pznotifications);
	MAPI_G(hr) = EC_SUCCESS;
	return;
RETURN_EXCEPTION:
	sleep(1);
	zval_ptr_dtor(&pznotifications);
	array_init(&pznotifications);
	RETVAL_ZVAL(&pznotifications, 0, 0);
	zval_ptr_dtor(&pznotifications);
}

ZEND_FUNCTION(mapi_table_queryallrows)
{
	zval pzrowset;
	uint32_t result;
	zval *pzresource;
	zval *pzproptags;
	TARRAY_SET rowset;
	zval *pzrestriction;
	MAPI_RESOURCE *ptable;
	PROPTAG_ARRAY proptags;
	RESTRICTION restriction;
	PROPTAG_ARRAY *pproptags;
	RESTRICTION *prestriction;
	
	ZVAL_NULL(&pzrowset);
	pzproptags = NULL;
	pzrestriction = NULL;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|aa",
		&pzresource, &pzproptags, &pzrestriction) == FAILURE ||
		NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (NULL != pzrestriction) {
		if (!php_to_restriction(pzrestriction, &restriction TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		prestriction = &restriction;
	} else {
		prestriction = NULL;
	}
	if (NULL != pzproptags) {
		if (!php_to_proptag_array(pzproptags, &proptags TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		pproptags = &proptags;
	} else {
		pproptags = NULL;
	}
	result = zarafa_client_queryrows(ptable->hsession,
		ptable->hobject, 0, 0x7FFFFFFF, prestriction,
		pproptags, &rowset);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (!tarray_set_to_php(&rowset, &pzrowset TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_ZVAL(&pzrowset, 0, 0);
	zval_ptr_dtor(&pzrowset);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_queryrows)
{
	long start;
	long row_count;
	zval pzrowset;
	uint32_t result;
	zval *pzresource;
	zval *pzproptags;
	TARRAY_SET rowset;
	MAPI_RESOURCE *ptable;
	PROPTAG_ARRAY proptags;
	PROPTAG_ARRAY *pproptags;
	
	ZVAL_NULL(&pzrowset);
	start = 0xFFFFFFFF;
	row_count = 0xFFFFFFFF;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|a!ll",
		&pzresource, &pzproptags, &start, &row_count) == FAILURE ||
		NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (NULL != pzproptags) {
		if (!php_to_proptag_array(pzproptags, &proptags TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		pproptags = &proptags;
	} else {
		pproptags = NULL;
	}
	result = zarafa_client_queryrows(ptable->hsession,
			ptable->hobject, start, row_count, NULL,
			pproptags, &rowset);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (!tarray_set_to_php(&rowset, &pzrowset TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_ZVAL(&pzrowset, 0, 0);
	zval_ptr_dtor(&pzrowset);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_setcolumns)
{
	long flags;
	uint32_t result;
	zval *pzresource;
	zval *pzproptags;
	MAPI_RESOURCE *ptable;
	PROPTAG_ARRAY proptags;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra|l",
		&pzresource, &pzproptags, &flags) == FAILURE || NULL ==
		pzresource || NULL == pzproptags) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_proptag_array(pzproptags, &proptags TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_setcolumns(
		ptable->hsession, ptable->hobject,
		&proptags, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_seekrow)
{
	long bookmark;
	long row_count;
	uint32_t result;
	zval *pzresource;
	int32_t rows_sought;
	MAPI_RESOURCE *ptable;
	
	row_count = 0;
	bookmark = BOOKMARK_BEGINNING;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rll",
		&pzresource, &bookmark, &row_count) == FAILURE || NULL
		== pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_seekrow(ptable->hsession,
		ptable->hobject, bookmark, row_count, &rows_sought);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_LONG(rows_sought);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_sort)
{
	long flags;
	uint32_t result;
	zval *pzresource;
	zval *pzsortarray;
	MAPI_RESOURCE *ptable;
	SORTORDER_SET sortcriteria;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra|l",
		&pzresource, &pzsortarray, &flags) == FAILURE || NULL ==
		pzresource || NULL == pzsortarray) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_sortorder_set(pzsortarray, &sortcriteria TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_sorttable(ptable->hsession,
					ptable->hobject, &sortcriteria);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_getrowcount)
{
	uint32_t count;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *ptable;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getrowcount(
		ptable->hsession, ptable->hobject,
		&count);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_LONG(count);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_restrict)
{
	ulong flags;
	uint32_t result;
	zval *pzresource;
	zval *pzrestrictarray;
	MAPI_RESOURCE *ptable;
	RESTRICTION restriction;

	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra|l",
		&pzresource, &pzrestrictarray, &flags) == FAILURE ||
		NULL == pzresource || NULL == pzrestrictarray || 0 ==
		zend_hash_num_elements(Z_ARRVAL_P(pzrestrictarray))) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_restriction(pzrestrictarray, &restriction TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_restricttable(
		ptable->hsession, ptable->hobject,
		&restriction, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_findrow)
{
	ulong flags;
	ulong bookmark;
	uint32_t result;
	uint32_t row_idx;
	zval *pzresource;
	zval *pzrestrictarray;
	MAPI_RESOURCE *ptable;
	RESTRICTION restriction;
	
	flags = 0;
	bookmark = BOOKMARK_BEGINNING;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra|ll",
		&pzresource, &pzrestrictarray, &bookmark, &flags) == FAILURE
		|| NULL == pzresource || NULL == pzrestrictarray || 0 ==
		zend_hash_num_elements(Z_ARRVAL_P(pzrestrictarray))) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_restriction(pzrestrictarray, &restriction TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_findrow(ptable->hsession,
			ptable->hobject, bookmark, &restriction,
			flags, &row_idx);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_LONG(row_idx);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_createbookmark)
{
	uint32_t result;
	zval *pzresource;
	uint32_t bookmark;
	MAPI_RESOURCE *ptable;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r",
		&pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_createbookmark(
		ptable->hsession, ptable->hobject,
		&bookmark);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_LONG(bookmark);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_table_freebookmark)
{
	long bookmark;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *ptable;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl",
		&pzresource, &bookmark) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(ptable, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_table, le_mapi_table);
	if (MAPI_TABLE != ptable->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_freebookmark(
		ptable->hsession, ptable->hobject,
		bookmark);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_msgstore_getreceivefolder)
{
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	uint32_t hobject;
	uint8_t mapi_type;
	MAPI_RESOURCE *pstore;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getreceivefolder(
			pstore->hsession, pstore->hobject,
			NULL, &entryid);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openstoreentry(
		pstore->hsession, pstore->hobject,
		entryid, 0, &mapi_type, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_FOLDER;
	presource->hsession = pstore->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_folder);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_modifyrecipients)
{
	long flags;
	uint32_t result;
	zval *pzadrlist;
	zval *pzresource;
	TARRAY_SET rcpt_list;
	MAPI_RESOURCE *pmessage;
	
	flags = MODRECIP_ADD;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rla", &pzresource, &flags, &pzadrlist) == FAILURE
		|| NULL == pzresource || NULL == pzadrlist) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_tarray_set(pzadrlist, &rcpt_list TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_modifyrecipients(
		pmessage->hsession, pmessage->hobject,
		flags, &rcpt_list);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_submitmessage)
{
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pmessage;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_submitmessage(
		pmessage->hsession, pmessage->hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_getattachmenttable)
{
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pmessage;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_loadattachmenttable(
			pmessage->hsession, pmessage->hobject,
			&hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_TABLE;
	presource->hsession = pmessage->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_table);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_openattach)
{
	long attach_id;
	uint32_t result;
	zval *pzresource;
	uint32_t hobject;
	MAPI_RESOURCE *pmessage;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl",
		&pzresource, &attach_id) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openattachment(
		pmessage->hsession, pmessage->hobject,
		attach_id, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_ATTACHMENT;
	presource->hsession = pmessage->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_attachment);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_createattach)
{
	long flags;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pmessage;
	MAPI_RESOURCE *presource;
	MAPI_RESOURCE *pattachment;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;	
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_createattachment(
		pmessage->hsession, pmessage->hobject,
		&hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_ATTACHMENT;
	presource->hsession = pmessage->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_attachment);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_deleteattach)
{	
	long flags;
	long attach_id;
	uint32_t result;
	zval *pzresource;
	uint32_t hobject;
	MAPI_RESOURCE *pmessage;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl|l",
		&pzresource, &attach_id, &flags) == FAILURE || NULL ==
		pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_deleteattachment(
		pmessage->hsession, pmessage->hobject,
		attach_id);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_read)
{
	void *pbuff;
	zval *pzresource;
	uint32_t actual_bytes;
	STREAM_OBJECT *pstream;
	unsigned long wanted_bytes;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rl", &pzresource, &wanted_bytes) == FAILURE ||
		NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresource, -1, name_stream, le_stream);
	pbuff = stream_object_read(pstream, wanted_bytes, &actual_bytes);
	if (NULL == pbuff) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_STRINGL(static_cast<const char *>(pbuff), actual_bytes);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_seek)
{
	long flags;
	zval *pzresource;
	long seek_offset;
	STREAM_OBJECT *pstream;
	
	flags = STREAM_SEEK_CUR;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl|l",
		&pzresource, &seek_offset, &flags) == FAILURE || NULL ==
		pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresource, -1, name_stream, le_stream);
	if (!stream_object_seek(pstream, flags, seek_offset)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_setsize)
{
	long newsize;
	zval *pzresource;
	STREAM_OBJECT *pstream;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rl", &pzresource, &newsize) == FAILURE || NULL
		== pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresource, -1, name_stream, le_stream);
	if (!stream_object_set_length(pstream, newsize)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_commit)
{
	uint32_t result;
	zval *pzresource;
	STREAM_OBJECT *pstream;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresource, -1, name_stream, le_stream);
	result = stream_object_commit(pstream);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_write)
{
	zval *pzresource;
	BINARY data_block;
	uint32_t written_len;
	STREAM_OBJECT *pstream;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs",
		&pzresource, &data_block.pb, &data_block.cb) == FAILURE
		|| NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresource, -1, name_stream, le_stream);
	written_len = stream_object_write(pstream,
				data_block.pb, data_block.cb);
	RETVAL_LONG(written_len);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_stat)
{
	zval *pzresource;
	uint32_t stream_size;
	STREAM_OBJECT *pstream;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresource, -1, name_stream, le_stream);
	stream_size = stream_object_get_length(pstream);
	array_init(return_value);
	add_assoc_long(return_value, "cb", stream_size);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_stream_create)
{
	STREAM_OBJECT *pstream;
	
	pstream = stream_object_create();
	if (NULL == pstream) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	ZEND_REGISTER_RESOURCE(return_value, pstream, le_stream);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_openpropertytostream)
{
	int	type;
	long flags;
	int guidlen;
	void *pvalue;
	long proptag;
	char *guidstr;
	uint32_t result;
	zval *pzresource;
	STREAM_OBJECT *pstream;
	MAPI_RESOURCE *probject;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rl|ls", &pzresource, &proptag, &flags, &guidstr,
		&guidlen) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	switch (proptag & 0xFFFF) {
	case PROPVAL_TYPE_BINARY:
	case PROPVAL_TYPE_STRING:
	case PROPVAL_TYPE_WSTRING:
		break;
	default:
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if(type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
		if (MAPI_STORE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_mailuser) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_mailuser, le_mapi_mailuser);
		if (MAPI_MAILUSER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	pstream = stream_object_create();
	if (NULL == pstream) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	stream_object_set_parent(
		pstream, probject->hsession,
		probject->hobject, proptag);
	result = zarafa_client_getpropval(probject->hsession,
		probject->hobject, phptag_to_proptag(proptag), &pvalue);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (NULL != pvalue) {
		if (PROPVAL_TYPE_BINARY == (proptag & 0xFFFF)) {
			stream_object_write(pstream,
				((BINARY*)pvalue)->pb, ((BINARY*)pvalue)->cb);
		} else {
			stream_object_write(pstream, pvalue, strlen(static_cast<const char *>(pvalue)));
		}
		stream_object_seek(pstream, STREAM_SEEK_SET, 0);
	}
	ZEND_REGISTER_RESOURCE(return_value, pstream, le_stream);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_getrecipienttable)
{
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pmessage;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_loadrecipienttable(
		pmessage->hsession, pmessage->hobject,
		&hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_TABLE;
	presource->hsession = pmessage->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_table);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_message_setreadflag)
{
	long flags;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *pmessage;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_setmessagereadflag(
		pmessage->hsession, pmessage->hobject,
		flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_attach_openobj)
{
	long flags;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pattach;
	MAPI_RESOURCE *presource;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pattach, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_attachment,
		le_mapi_attachment);
	if (MAPI_ATTACHMENT != pattach->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_openembedded(
		pattach->hsession, pattach->hobject,
		flags, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_MESSAGE;
	presource->hsession = pattach->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_message);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_getidsfromnames)
{
	int i;
	zval *pzstore;
	zval *pznames;
	zval *pzguids;
	uint32_t result;
	PROPID_ARRAY propids;
	MAPI_RESOURCE *pstore;
	PROPNAME_ARRAY propnames;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra|a", &pzstore, &pznames, &pzguids) == FAILURE
		|| NULL == pzstore || NULL == pznames) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzstore, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_propname_array(pznames,
		pzguids, &propnames TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getnamedpropids(
		pstore->hsession, pstore->hobject,
		&propnames, &propids);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	array_init(return_value);
	for (i=0; i<propids.count; i++) {
		add_next_index_long(return_value,
			((uint32_t)propids.ppropid[i]) << 16);
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_setprops)
{
	int type;
	uint32_t result;
	zval *pzpropvals;
	zval *pzresource;
	MAPI_RESOURCE *probject;
	TPROPVAL_ARRAY propvals;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra", &pzresource, &pzpropvals) == FAILURE ||
		NULL == pzresource || NULL == pzpropvals) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if(type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
		if (MAPI_STORE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_property) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_property, le_mapi_property);
		if (MAPI_PROFPROPERTY != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	if (!php_to_tpropval_array(pzpropvals, &propvals TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_setpropvals(probject->hsession,
							probject->hobject, &propvals);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_copyto)
{
	int type;
	int type1;
	long flags;
	zval *pzsrc;
	zval *pzdst;
	uint32_t result;
	zval *pzexcludeiids;
	zval *pzexcludeprops;
	MAPI_RESOURCE *psrcobject;
	MAPI_RESOURCE *pdstobject;
	PROPTAG_ARRAY exclude_proptags;
	PROPTAG_ARRAY *pexclude_proptags;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "raar|l",
		&pzsrc, &pzexcludeiids, &pzexcludeprops, &pzdst, &flags)
		== FAILURE || NULL == pzsrc || NULL == pzdst) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type  = Z_RES_TYPE_P(pzsrc);
	auto type1 = Z_RES_TYPE_P(pzdst);
	if (type != type1) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	if(type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(psrcobject, MAPI_RESOURCE*,
			&pzsrc, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != psrcobject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
		ZEND_FETCH_RESOURCE(pdstobject, MAPI_RESOURCE*,
			&pzdst, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != pdstobject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(psrcobject, MAPI_RESOURCE*,
			&pzsrc, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != psrcobject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
		ZEND_FETCH_RESOURCE(pdstobject, MAPI_RESOURCE*,
			&pzdst, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != pdstobject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(psrcobject, MAPI_RESOURCE*,
			&pzsrc, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != psrcobject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
		ZEND_FETCH_RESOURCE(pdstobject, MAPI_RESOURCE*,
			&pzdst, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != pdstobject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	if (NULL == pzexcludeprops) {
		pexclude_proptags = NULL;
	} else {
		if (!php_to_proptag_array(pzexcludeprops,
			&exclude_proptags TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		pexclude_proptags = &exclude_proptags;
	}
	result = zarafa_client_copyto(psrcobject->hsession,
				psrcobject->hobject, pexclude_proptags,
				pdstobject->hobject, flags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_savechanges)
{
	int type;
	long flags;
	uint32_t result;
	zval *pzresource;
	MAPI_RESOURCE *probject;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
		if (MAPI_STORE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_property) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_property, le_mapi_property);
		if (MAPI_PROFPROPERTY != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	switch (probject->type) {
	case MAPI_ATTACHMENT:
	case MAPI_MESSAGE:
		result = zarafa_client_savechanges(
			probject->hsession, probject->hobject);
		if (EC_SUCCESS != result) {
			MAPI_G(hr) = result;
			goto THROW_EXCEPTION;
		}
		break;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_deleteprops)
{
	int type;
	uint32_t result;
	zval *pzresource;
	zval *pzproptags;
	PROPTAG_ARRAY proptags;
	MAPI_RESOURCE *probject;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra", &pzresource, &pzproptags) == FAILURE ||
		NULL == pzresource || NULL == pzproptags) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if(type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
		if (MAPI_STORE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	}
	if (!php_to_proptag_array(pzproptags, &proptags TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_deletepropvals(probject->hsession,
								probject->hobject, &proptags);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_openproperty)
{
	int type;
	int flags;
	int guidlen;
	long proptag;
	void *pvalue;
	char *guidstr;
	GUID iid_guid;
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	PULL_CTX pull_ctx;
	long interfaceflags;
	STREAM_OBJECT *pstream;
	MAPI_RESOURCE *probject;
	MAPI_RESOURCE *presource;
	ICS_IMPORT_CTX *pimporter;
	ICS_EXPORT_CTX *pexporter;
	
	flags = 0;
	if (ZEND_NUM_ARGS() == 2) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
			"rl", &pzresource, &proptag) == FAILURE || NULL
			== pzresource) {
			MAPI_G(hr) = EC_INVALID_PARAMETER;
			goto THROW_EXCEPTION;
		}
		flags = 0;
		interfaceflags = 0;
		iid_guid = IID_IStream;
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
			"rlsll", &pzresource, &proptag, &guidstr,
			&guidlen, &interfaceflags, &flags) == FAILURE ||
			NULL == pzresource || NULL == guidstr) {
			MAPI_G(hr) = EC_INVALID_PARAMETER;
			goto THROW_EXCEPTION;
		}
		ext_pack_pull_init(&pull_ctx, reinterpret_cast<const uint8_t *>(guidstr), sizeof(GUID));
		ext_pack_pull_guid(&pull_ctx, &iid_guid);
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_message, le_mapi_message);
		if (MAPI_MESSAGE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_attachment, le_mapi_attachment);
		if (MAPI_ATTACHMENT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
		if (MAPI_STORE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_NOT_SUPPORTED;
		goto THROW_EXCEPTION;
	}
	}
	if (0 == memcmp(&iid_guid, &IID_IStream, sizeof(GUID))) {
		switch (proptag & 0xFFFF) {
		case PROPVAL_TYPE_BINARY:
		case PROPVAL_TYPE_STRING:
		case PROPVAL_TYPE_WSTRING:
			break;
		default:
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		result = zarafa_client_getpropval(probject->hsession,
			probject->hobject, phptag_to_proptag(proptag), &pvalue); /* memleak(pvalue) */
		if (EC_SUCCESS != result) {
			MAPI_G(hr) = result;
			goto THROW_EXCEPTION;
		}
		if (ZEND_NUM_ARGS() == 2) {
			if (NULL == pvalue) {
				MAPI_G(hr) = EC_NOT_FOUND;
				goto THROW_EXCEPTION;
			}
			if (PROPVAL_TYPE_BINARY == proptag & 0xFFFF) {
				RETVAL_STRINGL(reinterpret_cast<const char *>(static_cast<BINARY *>(pvalue)->pb), static_cast<BINARY *>(pvalue)->cb);
			} else {
				RETVAL_STRINGL(static_cast<const char *>(pvalue), strlen(static_cast<const char *>(pvalue)));
			}
		} else {
			pstream = stream_object_create();
			if (NULL == pstream) {
				MAPI_G(hr) = EC_OUT_OF_MEMORY;
				goto THROW_EXCEPTION;
			}
			stream_object_set_parent(
				pstream, probject->hsession,
				probject->hobject, proptag);
			if (NULL != pvalue) {
				if (PROPVAL_TYPE_BINARY == (proptag & 0xFFFF)) {
					stream_object_write(pstream,
						((BINARY*)pvalue)->pb,
						((BINARY*)pvalue)->cb);
				} else {
					stream_object_write(pstream,
						pvalue, strlen(static_cast<const char *>(pvalue)));
				}
				stream_object_seek(pstream, STREAM_SEEK_SET, 0);
			}
			ZEND_REGISTER_RESOURCE(return_value, pstream, le_stream);
		}
	} else if (0 == memcmp(&iid_guid, &IID_IMessage, sizeof(GUID))) {
		if (type != le_mapi_attachment ||
			PR_ATTACH_DATA_OBJ != proptag) {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		result = zarafa_client_openembedded(probject->hsession,
							probject->hobject, flags, &hobject);
		if (EC_SUCCESS != result) {
			MAPI_G(hr) = result;
			goto THROW_EXCEPTION;
		}
		presource = st_malloc<MAPI_RESOURCE>();
		if (NULL == presource) {
			MAPI_G(hr) = EC_OUT_OF_MEMORY;
			goto THROW_EXCEPTION;
		}
		presource->type = MAPI_MESSAGE;
		presource->hsession = probject->hsession;
		presource->hobject = hobject;
		ZEND_REGISTER_RESOURCE(return_value, presource, le_mapi_message);
	} else if (0 == memcmp(&iid_guid,
		&IID_IExchangeExportChanges, sizeof(GUID))) {
		if (type != le_mapi_folder) {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		if (PR_CONTENTS_SYNCHRONIZER == proptag) {
			result = zarafa_client_contentsync(probject->hsession,
									probject->hobject, &hobject);
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
		} else if (PR_HIERARCHY_SYNCHRONIZER == proptag) {
			result = zarafa_client_hierarchysync(probject->hsession,
										probject->hobject, &hobject);
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
		} else {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		pexporter = st_malloc<ICS_EXPORT_CTX>();
		if (NULL == pexporter) {
			MAPI_G(hr) = EC_OUT_OF_MEMORY;
			goto THROW_EXCEPTION;
		}
		pexporter->hsession = probject->hsession;
		pexporter->hobject = hobject;
		ZVAL_NULL(&pexporter->pztarget_obj);
		if (PR_CONTENTS_SYNCHRONIZER == proptag) {
			pexporter->ics_type = ICS_TYPE_CONTENTS;
		} else {
			pexporter->ics_type = ICS_TYPE_HIERARCHY;
		}
		pexporter->progress = 0;
		pexporter->sync_steps = 0;
		pexporter->total_steps = 0;
		ZEND_REGISTER_RESOURCE(return_value,
			pexporter, le_mapi_exportchanges);
	} else if (0 == memcmp(&iid_guid,
		&IID_IExchangeImportHierarchyChanges, sizeof(GUID))) {
		if (type != le_mapi_folder) {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		if (PR_COLLECTOR != proptag) {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		result = zarafa_client_hierarchyimport(
			probject->hsession, probject->hobject,
			&hobject);
		if (EC_SUCCESS != result) {
			MAPI_G(hr) = result;
			goto THROW_EXCEPTION;	
		}
		pimporter = st_malloc<ICS_IMPORT_CTX>();
		if (NULL == pimporter) {
			MAPI_G(hr) = EC_OUT_OF_MEMORY;
			goto THROW_EXCEPTION;
		}
		pimporter->hsession = probject->hsession;
		pimporter->hobject = hobject;
		ZVAL_NULL(&pimporter->pztarget_obj);
		pimporter->ics_type = ICS_TYPE_HIERARCHY;
		ZEND_REGISTER_RESOURCE(return_value,
			pimporter, le_mapi_importhierarchychanges);
	} else if(0 == memcmp(&iid_guid,
		&IID_IExchangeImportContentsChanges,
		sizeof(GUID))) {
		if (type != le_mapi_folder) {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		if (PR_COLLECTOR != proptag) {
			MAPI_G(hr) = EC_NOT_SUPPORTED;
			goto THROW_EXCEPTION;
		}
		result = zarafa_client_contentimport(
			probject->hsession, probject->hobject,
			&hobject);
		if (EC_SUCCESS != result) {
			MAPI_G(hr) = result;
			goto THROW_EXCEPTION;	
		}
		pimporter = st_malloc<ICS_IMPORT_CTX>();
		if (NULL == pimporter) {
			MAPI_G(hr) = EC_OUT_OF_MEMORY;
			goto THROW_EXCEPTION;
		}
		pimporter->hsession = probject->hsession;
		pimporter->hobject = hobject;
		ZVAL_NULL(&pimporter->pztarget_obj);
		pimporter->ics_type = ICS_TYPE_CONTENTS;
		ZEND_REGISTER_RESOURCE(return_value,
			pimporter, le_mapi_importcontentschanges);
	} else {
		MAPI_G(hr) = EC_NOT_SUPPORTED;
		goto THROW_EXCEPTION;
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_getprops)
{
	int type;
	uint32_t result;
	zval *pzresource;
	zval *pztagarray;
	zval pzpropvals;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	MAPI_RESOURCE *probject;
	PROPTAG_ARRAY *pproptags;
	
	ZVAL_NULL(&pzpropvals);
	pztagarray = NULL;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r|a", &pzresource, &pztagarray) == FAILURE ||
		NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_message) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
				&pzresource, -1, name_mapi_message,
				le_mapi_message);
		if (MAPI_MESSAGE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
					&pzresource, -1, name_mapi_folder,
					le_mapi_folder);
		if (MAPI_FOLDER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_attachment) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
				&pzresource, -1, name_mapi_attachment,
				le_mapi_attachment);
		if (MAPI_ATTACHMENT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
				&pzresource, -1, name_mapi_msgstore,
				le_mapi_msgstore);
		if (MAPI_STORE != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_mailuser) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
				&pzresource, -1, name_mapi_mailuser,
				le_mapi_mailuser);
		if (MAPI_MAILUSER != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_distlist) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
				&pzresource, -1, name_mapi_distlist,
				le_mapi_distlist);
		if (MAPI_DISTLIST != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_abcont) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
					&pzresource, -1, name_mapi_abcont,
					le_mapi_abcont);
		if (MAPI_ABCONT != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_property) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
				&pzresource, -1, name_mapi_property,
				le_mapi_property);
		if (MAPI_PROFPROPERTY != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_addressbook) {
		ZEND_FETCH_RESOURCE(probject, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_addressbook,
			le_mapi_addressbook);
		if (MAPI_ADDRESSBOOK != probject->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_NOT_SUPPORTED;
		goto THROW_EXCEPTION;
	}
	}
	if(NULL != pztagarray) {
		if (!php_to_proptag_array(pztagarray,
			&proptags TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		pproptags = &proptags;
	} else {
		pproptags = NULL;
	}
	result = zarafa_client_getpropvals(probject->hsession,
				probject->hobject, pproptags, &propvals);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (!tpropval_array_to_php(&propvals,
		&pzpropvals TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	RETVAL_ZVAL(&pzpropvals, 0, 0);
	zval_ptr_dtor(&pzpropvals);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_getnamesfromids)
{
	int i;
	zval *pzprop;
	zval *pzarray;
	uint32_t result;
	zval *pzresource;
	char num_buff[20];
	PROPID_ARRAY propids;
	MAPI_RESOURCE *pstore;
	PROPTAG_ARRAY proptags;
	PROPNAME_ARRAY propnames;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra", &pzresource, &pzarray) == FAILURE || NULL
		== pzresource || NULL == pzarray) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstore, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
	if (MAPI_STORE != pstore->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	if (!php_to_proptag_array(pzarray, &proptags TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	propids.count = proptags.count;
	propids.ppropid = sta_malloc<uint16_t>(proptags.count);
	if (NULL == propids.ppropid) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	for (i=0; i<proptags.count; i++) {
		propids.ppropid[i] = proptags.pproptag[i] >> 16;
	}
	result = zarafa_client_getpropnames(
		pstore->hsession, pstore->hobject,
		&propids, &propnames);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	array_init(return_value);
	for (i=0; i<propnames.count; i++) {
		if (KIND_NONE == propnames.ppropname[i].kind) {
			continue;
		}
		snprintf(num_buff, 20, "%i", proptag_to_phptag(proptags.pproptag[i]));
		zval prop, *pzprop = &prop;
		array_init(pzprop);
		add_assoc_stringl(pzprop, "guid",
			(char*)&propnames.ppropname[i].guid,
			sizeof(GUID));
		if (KIND_LID == propnames.ppropname[i].kind) {
			add_assoc_long(pzprop, "id",
				*propnames.ppropname[i].plid);
		} else {
			add_assoc_string(pzprop, "name",
				propnames.ppropname[i].pname);
		}
		add_assoc_zval(return_value, num_buff, pzprop);
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_decompressrtf)
{
	pid_t pid;
	int status;
	int offset;
	int rtflen;
	int bufflen;
	int readlen;
	char *pbuff;
	char* args[2];
	char *rtfbuffer;
	int pipes_in[2] = {-1, -1};
	int pipes_out[2] = {-1, -1};
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"s", &rtfbuffer, &rtflen) == FAILURE || NULL ==
		rtfbuffer || 0 == rtflen) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	if (-1 == pipe(pipes_in)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	if (-1 == pipe(pipes_out)) {
		close(pipes_in[0]);
		close(pipes_in[1]);
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	pid = fork();
	switch (pid) {
	case 0:
		close(pipes_in[1]);
		close(pipes_out[0]);
		close(0);
		close(1);
		dup2(pipes_in[0], 0);
		dup2(pipes_out[1], 1);
		close(pipes_in[0]);
		close(pipes_out[1]);
		chdir(TOOLS_PATH);
		args[0] = const_cast<char *>("./rtf2html");
		args[1] = NULL;
		execv("./rtf2html", args);
		_exit(-1);
	 case -1:
		close(pipes_in[0]);
		close(pipes_in[1]);
		close(pipes_out[0]);
		close(pipes_out[1]);
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	default:
		close(pipes_in[0]);
		close(pipes_out[1]);
		break;
	}
	write(pipes_in[1], rtfbuffer, rtflen);
	close(pipes_in[1]);
	offset = 0;
	bufflen = 64*1024;
	pbuff = sta_malloc<char>(bufflen);
	if (NULL == pbuff) {
		close(pipes_out[0]);
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	while ((readlen = read(pipes_out[0],
		pbuff, bufflen - offset)) > 0) {
		offset += readlen;
		if (offset == bufflen) {
			bufflen *= 2;
			pbuff = sta_realloc<char>(pbuff, bufflen);
			if (NULL == pbuff) {
				close(pipes_out[0]);
				MAPI_G(hr) = EC_OUT_OF_MEMORY;
				goto THROW_EXCEPTION;
			}
		}
	}
	waitpid(pid, &status, 0);
	close(pipes_out[0]);
	RETVAL_STRINGL(pbuff, offset);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_getrulestable)
{
	uint32_t result;
	uint32_t hobject;
	zval *pzresource;
	MAPI_RESOURCE *pfolder;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_loadruletable(
		pfolder->hsession, pfolder->hobject,
		&hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_TABLE;
	presource->hsession = pfolder->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(return_value,
				presource, le_mapi_table);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_getsearchcriteria)
{
	long flags;
	uint32_t result;
	zval *pzresource;
	zval pzfolderlist, pzrestriction;
	uint32_t search_state;
	MAPI_RESOURCE *pfolder;
	RESTRICTION *prestriction;
	BINARY_ARRAY entryid_array;

	ZVAL_NULL(&pzfolderlist);
	ZVAL_NULL(&pzrestriction);
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l",
		&pzresource, &flags) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getsearchcriteria(
		pfolder->hsession, pfolder->hobject,
		&entryid_array, &prestriction,
		&search_state);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (NULL == prestriction) {
		ZVAL_NULL(&pzrestriction);
	} else if (!restriction_to_php(
		prestriction, &pzrestriction TSRMLS_CC)
		|| !binary_array_to_php(&entryid_array,
		&pzfolderlist TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	array_init(return_value);
	add_assoc_zval(return_value, "restriction", &pzrestriction);
	add_assoc_zval(return_value, "folderlist", &pzfolderlist);
	add_assoc_long(return_value, "searchstate", search_state);
	zval_ptr_dtor(&pzrestriction);
	zval_ptr_dtor(&pzfolderlist);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_setsearchcriteria)
{
	long flags;
	uint32_t result;
	zval *pzresource;
	zval *pzfolderlist;
	zval *pzrestriction;
	MAPI_RESOURCE *pfolder;
	RESTRICTION restriction;
	RESTRICTION *prestriction;
	BINARY_ARRAY entryid_array;
	BINARY_ARRAY *pentryid_array;
	
	flags = 0;
	if (!zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "raal",
		&pzresource, &pzrestriction, &pzfolderlist, &flags) ==
		FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (NULL == pzrestriction) {
		prestriction = NULL;
	} else {
		if (!php_to_restriction(pzrestriction,
			&restriction TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		prestriction = &restriction;
	}
	if (NULL == pzfolderlist) {
		pentryid_array = NULL;
	} else {
		if (!php_to_binary_array(pzfolderlist,
			&entryid_array TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		pentryid_array = &entryid_array;
	}
	result = zarafa_client_setsearchcriteria(
		pfolder->hsession, pfolder->hobject,
		flags, &entryid_array, &restriction);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_folder_modifyrules)
{
	long flags;
	zval *pzrows;
	uint32_t result;
	zval *pzresource;
	RULE_LIST rule_list;
	MAPI_RESOURCE *pfolder;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra|l", &pzresource, &pzrows, &flags) == FAILURE
		|| NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_folder, le_mapi_folder);
	if (MAPI_FOLDER != pfolder->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	if (!php_to_rule_list(pzrows, &rule_list TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_modifyrules(pfolder->hsession,
					pfolder->hobject, flags, &rule_list);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_zarafa_getpermissionrules)
{
	int i;
	int type;
	long acl_type;
	uint32_t result;
	zval *pzresource;
	zval *pzdata_value;
	PERMISSION_SET perm_set;
	MAPI_RESOURCE *presource;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rl", &pzresource, &acl_type) == FAILURE || NULL
		== pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	if (ACCESS_TYPE_GRANT != acl_type) {
		MAPI_G(hr) = EC_NOT_SUPPORTED;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_msgstore) {
		ZEND_FETCH_RESOURCE(presource, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_msgstore, le_mapi_msgstore);
		if (MAPI_STORE != presource->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(presource, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != presource->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_NOT_SUPPORTED;
		goto THROW_EXCEPTION;
	}
	}
	result = zarafa_client_getpermissions(
		presource->hsession, presource->hobject,
		&perm_set);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	array_init(return_value);
	for (i=0; i<perm_set.count; i++) {
		zval pzdata_value;
		array_init(&pzdata_value);
		add_assoc_stringl(&pzdata_value, "userid",
			reinterpret_cast<const char *>(perm_set.prows[i].entryid.pb),
			perm_set.prows[i].entryid.cb);
		add_assoc_long(&pzdata_value,
			"type", ACCESS_TYPE_GRANT);
		add_assoc_long(&pzdata_value, "rights",
			perm_set.prows[i].member_rights);
		add_assoc_long(&pzdata_value,
			"state", RIGHT_NORMAL);
		add_index_zval(return_value, i, &pzdata_value);
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_zarafa_setpermissionrules)
{
	int i, j;
	int type;
	zval *pzperms;
	uint32_t result;
	zval **ppzentry;
	zval **ppzvalue;
	zval *pzresource;
	HashTable *pdata;
	LONG_ARRAY uid_array;
	MAPI_RESOURCE *pfolder;
	PERMISSION_SET perm_set;
	HashTable *ptarget_hash;
	zstrplus str_userid(zend_string_init(ZEND_STRL("userid"), 0));
	zstrplus str_type(zend_string_init(ZEND_STRL("type"), 0));
	zstrplus str_rights(zend_string_init(ZEND_STRL("rights"), 0));
	zstrplus str_state(zend_string_init(ZEND_STRL("state"), 0));

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra", &pzresource, &pzperms) == FAILURE || NULL
		== pzresource || NULL == pzperms) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	{
	auto type = Z_RES_TYPE_P(pzresource);
	if (type == le_mapi_folder) {
		ZEND_FETCH_RESOURCE(pfolder, MAPI_RESOURCE*,
			&pzresource, -1, name_mapi_folder, le_mapi_folder);
		if (MAPI_FOLDER != pfolder->type) {
			MAPI_G(hr) = EC_INVALID_OBJECT;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_NOT_SUPPORTED;
		goto THROW_EXCEPTION;
	}
	}
	ptarget_hash = HASH_OF(pzperms);
	if (NULL == ptarget_hash) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	zend_hash_internal_pointer_reset(ptarget_hash);
	perm_set.count = zend_hash_num_elements(ptarget_hash);
	perm_set.prows = sta_malloc<PERMISSION_ROW>(perm_set.count);
	if (NULL == perm_set.prows) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}

	for (i=0,j=0; i<perm_set.count; i++) {
		auto ppzentry = zend_hash_get_current_data(ptarget_hash);
		pdata = HASH_OF(ppzentry);
		zend_hash_internal_pointer_reset(pdata);
		auto value = zend_hash_find(pdata, str_userid.get());
		if (value == nullptr)
			continue;
		auto ss = zval_get_string(value);
		perm_set.prows[j].entryid.pb = reinterpret_cast<uint8_t *>(ZSTR_VAL(ss));
		perm_set.prows[j].entryid.cb = ZSTR_LEN(ss);
		value = zend_hash_find(pdata, str_type.get());
		if (value == nullptr)
			continue;
		if (zval_get_long(value) != ACCESS_TYPE_GRANT) {
			continue;
		}
		value = zend_hash_find(pdata, str_rights.get());
		if (value == nullptr)
			continue;
		perm_set.prows[j].member_rights = zval_get_long(value);
		value = zend_hash_find(pdata, str_state.get());
		if (value != nullptr) {
			perm_set.prows[j].flags = zval_get_long(value);
		} else {
			perm_set.prows[j].flags = RIGHT_NEW|RIGHT_AUTOUPDATE_DENIED;
		}
		j ++;
		zend_hash_move_forward(ptarget_hash);
	}
	result = zarafa_client_modifypermissions(
		pfolder->hsession, pfolder->hobject,
		&perm_set);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	MAPI_G(hr) = EC_SUCCESS;
	RETVAL_TRUE;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

/*
	This function will get user's freebusy data
	
	param session[in]	session object
	param entryid[in]	user's entryid
	param starttime		unix time stamp
	param endtime 		unix time stamp
	return				json string of user's freebusy data,
						json string, empty string means not
						found. fileds:
						starttime, endtime, busytype, subject(base64),
						location(base64), rests are all bool(absense
						means false). ismeeting, isrecurring,
						isexception, isreminderset, isprivate
*/
ZEND_FUNCTION(mapi_getuseravailability)
{
	long endtime;
	long starttime;
	BINARY entryid;
	uint32_t result;
	zval *pzresource;
	char *presult_string;
	MAPI_RESOURCE *psession;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rsll",
		&pzresource, &entryid.pb, &entryid.cb, &starttime, &endtime)
		== FAILURE || NULL == pzresource || NULL == entryid.pb ||
		0 == entryid.cb) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_getuseravailability(
		psession->hsession, entryid, starttime,
		endtime, &presult_string);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	if (NULL == presult_string) {
		RETVAL_NULL();
		return;
	}
	RETVAL_STRING(presult_string);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_exportchanges_config)
{
	int type;
	long flags;
	long buffersize;
	uint32_t result;
	zval *pzrestrict;
	zval *pzresstream;
	zval *pzincludeprops;
	zval *pzexcludeprops;
	ICS_EXPORT_CTX *pctx;
	STREAM_OBJECT *pstream;
	RESTRICTION restriction;
	zval *pzresimportchanges;
	zval *pzresexportchanges;
	RESTRICTION *prestriction;
	ICS_IMPORT_CTX *pimporter;
	
	flags = 0;
	buffersize = 1;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rrlzzzzl", &pzresexportchanges, &pzresstream, &flags,
		&pzresimportchanges, &pzrestrict, &pzincludeprops,
		&pzexcludeprops, &buffersize) == FAILURE || NULL
		== pzresexportchanges || NULL == pzresstream ||
		NULL == pzresimportchanges) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	pimporter = NULL;
	ZEND_FETCH_RESOURCE(pctx, ICS_EXPORT_CTX*, &pzresexportchanges,
		-1, name_mapi_exportchanges, le_mapi_exportchanges);
	if (Z_TYPE_P(pzresimportchanges) == IS_RESOURCE) {
		auto type = Z_RES_TYPE_P(pzresimportchanges);
		if(type == le_mapi_importcontentschanges) {
			ZEND_FETCH_RESOURCE(pimporter,
				ICS_IMPORT_CTX*, &pzresimportchanges,
				-1, name_mapi_importcontentschanges,
				le_mapi_importcontentschanges);
		} else if(type == le_mapi_importhierarchychanges) {
			ZEND_FETCH_RESOURCE(pimporter,
				ICS_IMPORT_CTX*, &pzresimportchanges,
				-1, name_mapi_importhierarchychanges,
				le_mapi_importhierarchychanges);
		} else {
			MAPI_G(hr) = EC_INVALID_PARAMETER;
			goto THROW_EXCEPTION;
		}
	} else {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresstream, -1, name_stream, le_stream);
	if (NULL != pzrestrict && Z_TYPE_P(pzrestrict) == IS_ARRAY) {
		if (!php_to_restriction(pzrestrict, &restriction TSRMLS_CC)) {
			MAPI_G(hr) = EC_ERROR;
			goto THROW_EXCEPTION;
		}
		prestriction = &restriction;
	} else {
		prestriction = NULL;
	}
	zval_ptr_dtor(&pctx->pztarget_obj);
	pctx->sync_steps = buffersize;
	ZVAL_OBJ(&pctx->pztarget_obj, Z_OBJ(pimporter->pztarget_obj));
	Z_ADDREF(pctx->pztarget_obj);
#ifdef ZTS
	pctx->TSRMLS_C = pimporter->TSRMLS_C;
#endif
	pctx->ics_type = pimporter->ics_type;
	result = zarafa_client_configsync(pctx->hsession, pctx->hobject,
			flags, stream_object_get_content(pstream), prestriction,
			&pctx->b_changed, &pctx->total_steps);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

static zend_bool import_message_change(zval *pztarget_obj,
	const TPROPVAL_ARRAY *pproplist, uint32_t flags)
{
	uint32_t hresult;
	zval pzvalreturn, pzvalargs[3], pzvalfuncname;

	ZVAL_NULL(&pzvalargs[0]);
	ZVAL_LONG(&pzvalargs[1], flags);
	ZVAL_NULL(&pzvalargs[2]);
	if (!tpropval_array_to_php(pproplist,
		&pzvalargs[0] TSRMLS_CC)) {
		return 0;
	}
	ZVAL_NULL(&pzvalreturn);
	ZVAL_STRING(&pzvalfuncname, "ImportMessageChange");
	if (call_user_function(NULL, pztarget_obj, &pzvalfuncname,
	    &pzvalreturn, 3, pzvalargs TSRMLS_CC) == FAILURE) {
		hresult = EC_ERROR;
	} else {
		hresult = zval_get_long(&pzvalreturn);
	}
	zval_ptr_dtor(&pzvalfuncname);
	zval_ptr_dtor(&pzvalreturn);
	zval_ptr_dtor(&pzvalargs[0]);
	zval_ptr_dtor(&pzvalargs[1]);
	zval_ptr_dtor(&pzvalargs[2]);
	if (EC_IGNORE_FAILURE != hresult) {
		return 0;
	}
	return 1;
}

static zend_bool import_message_deletion(zval *pztarget_obj,
	uint32_t flags, const BINARY_ARRAY *pbins)
{
	zval pzvalreturn, pzvalargs[2], pzvalfuncname;

	ZVAL_NULL(&pzvalfuncname);
	ZVAL_NULL(&pzvalreturn);
	ZVAL_LONG(&pzvalargs[0], flags);
	ZVAL_NULL(&pzvalargs[1]);
	if (!binary_array_to_php(pbins, &pzvalargs[1] TSRMLS_CC)) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		zval_ptr_dtor(&pzvalargs[0]);
		return 0;
	}
	ZVAL_STRING(&pzvalfuncname, "ImportMessageDeletion");
	if (call_user_function(NULL, pztarget_obj, &pzvalfuncname,
	    &pzvalreturn, 2, pzvalargs TSRMLS_CC) == FAILURE) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		zval_ptr_dtor(&pzvalargs[0]);
		zval_ptr_dtor(&pzvalargs[1]);
		return 0;
	}
	zval_ptr_dtor(&pzvalfuncname);
	zval_ptr_dtor(&pzvalreturn);
	zval_ptr_dtor(&pzvalargs[0]);
	zval_ptr_dtor(&pzvalargs[1]);
	return 1;
}

static zend_bool import_readstate_change(
	zval *pztarget_obj, const STATE_ARRAY *pstates)
{
	zval pzvalargs, pzvalreturn, pzvalfuncname;
    
	ZVAL_NULL(&pzvalfuncname);
	ZVAL_NULL(&pzvalreturn);
	if (!state_array_to_php(pstates, &pzvalargs TSRMLS_CC)) {
		return 0;
	}
	ZVAL_STRING(&pzvalfuncname, "ImportPerUserReadStateChange");
	if (call_user_function(nullptr, pztarget_obj, &pzvalfuncname,
	    &pzvalreturn, 1, &pzvalargs TSRMLS_CC) == FAILURE) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		zval_ptr_dtor(&pzvalargs);
		return 0;
	}
	zval_ptr_dtor(&pzvalfuncname);
	zval_ptr_dtor(&pzvalreturn);
	zval_ptr_dtor(&pzvalargs);
	return 1;
}

static zend_bool import_folder_change(zval *pztarget_obj,
	TPROPVAL_ARRAY *pproplist)
{
	zval pzvalargs, pzvalreturn, pzvalfuncname;

	ZVAL_NULL(&pzvalfuncname);
	ZVAL_NULL(&pzvalreturn);
	if (!tpropval_array_to_php(pproplist,
		&pzvalargs TSRMLS_CC)) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		return 0;
	}
	ZVAL_STRING(&pzvalfuncname, "ImportFolderChange");
	if (call_user_function(nullptr, pztarget_obj, &pzvalfuncname,
	    &pzvalreturn, 1, &pzvalargs TSRMLS_CC) == FAILURE) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		zval_ptr_dtor(&pzvalargs);
		return 0;
	}
	zval_ptr_dtor(&pzvalfuncname);
	zval_ptr_dtor(&pzvalreturn);
	zval_ptr_dtor(&pzvalargs);
	return 1;
}

static zend_bool import_folder_deletion(zval *pztarget_obj,
	BINARY_ARRAY *pentryid_array)
{
	zval pzvalreturn, pzvalargs[2], pzvalfuncname;

	ZVAL_NULL(&pzvalfuncname);
	ZVAL_NULL(&pzvalreturn);
	ZVAL_LONG(&pzvalargs[0], 0); /* flags, not used currently */
	if (!binary_array_to_php(pentryid_array,
		&pzvalargs[1] TSRMLS_CC)) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		zval_ptr_dtor(&pzvalargs[0]);
		return 0;
	}
	ZVAL_STRING(&pzvalfuncname, "ImportFolderDeletion");
	if (call_user_function(nullptr, pztarget_obj, &pzvalfuncname,
	    &pzvalreturn, 2, pzvalargs TSRMLS_CC) == FAILURE) {
		zval_ptr_dtor(&pzvalfuncname);
		zval_ptr_dtor(&pzvalreturn);
		zval_ptr_dtor(&pzvalargs[0]);
		zval_ptr_dtor(&pzvalargs[1]);
		return 0;
	}
	zval_ptr_dtor(&pzvalfuncname);
	zval_ptr_dtor(&pzvalreturn);
	zval_ptr_dtor(&pzvalargs[0]);
	zval_ptr_dtor(&pzvalargs[1]);
	return 1;
}


ZEND_FUNCTION(mapi_exportchanges_synchronize)
{
	int i;
	uint32_t flags;
	zend_bool b_new;
	uint32_t result;
	zval *pzresource;
	BINARY_ARRAY bins;
	STATE_ARRAY states;
	ICS_EXPORT_CTX *pctx;
	TPROPVAL_ARRAY propvals;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx, ICS_EXPORT_CTX*, &pzresource,
		-1, name_mapi_exportchanges, le_mapi_exportchanges);
	if (0 == pctx->b_changed) {
		RETVAL_TRUE;
		MAPI_G(hr) = EC_SUCCESS;
		return;
	}
	if (0 == pctx->progress) {
		if (ICS_TYPE_CONTENTS == pctx->ics_type) {
			result = zarafa_client_syncdeletions(
				pctx->hsession, pctx->hobject, 0, &bins);
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
			if (bins.count > 0 && !import_message_deletion(&pctx->pztarget_obj, 0, &bins)) {
				MAPI_G(hr) = EC_ERROR;
				goto THROW_EXCEPTION;
			}
			result = zarafa_client_syncdeletions(pctx->hsession,
						pctx->hobject, SYNC_SOFT_DELETE, &bins);
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
			if (bins.count > 0 && !import_message_deletion(&pctx->pztarget_obj, SYNC_SOFT_DELETE, &bins)) {
				MAPI_G(hr) = EC_ERROR;
				goto THROW_EXCEPTION;
			}
			result = zarafa_client_syncreadstatechanges(
				pctx->hsession, pctx->hobject, &states);
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
			if (states.count > 0 && !import_readstate_change(&pctx->pztarget_obj, &states)) {
				MAPI_G(hr) = EC_ERROR;
				goto THROW_EXCEPTION;
			}
		} else {
			result = zarafa_client_syncdeletions(
				pctx->hsession, pctx->hobject, 0, &bins);
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
			if (bins.count > 0 && !import_folder_deletion(&pctx->pztarget_obj, &bins)) {
				MAPI_G(hr) = EC_ERROR;
				goto THROW_EXCEPTION;
			}
		}
	}
	for (i=0; i<pctx->sync_steps; i++,pctx->progress++) {
		if (ICS_TYPE_CONTENTS == pctx->ics_type) {
			result = zarafa_client_syncmessagechange(
				pctx->hsession, pctx->hobject, &b_new,
				&propvals);
			if (EC_NOT_FOUND == result) {
				continue;
			}
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
			if (b_new) {
				flags = SYNC_NEW_MESSAGE;
			} else {
				flags = 0;
			}
			if (!import_message_change(&pctx->pztarget_obj,
				&propvals, flags)) {
				MAPI_G(hr) = EC_ERROR;
				goto THROW_EXCEPTION;
			}
		} else {
			result = zarafa_client_syncfolderchange(
				pctx->hsession, pctx->hobject,
				&propvals);
			if (EC_NOT_FOUND == result) {
				continue;
			}
			if (EC_SUCCESS != result) {
				MAPI_G(hr) = result;
				goto THROW_EXCEPTION;
			}
			if (!import_folder_change(&pctx->pztarget_obj,
				&propvals)) {
				MAPI_G(hr) = EC_ERROR;
				goto THROW_EXCEPTION;
			}
		}
	}
	if (pctx->progress >= pctx->total_steps) {
		RETVAL_TRUE;
	} else {
		array_init(return_value);
		add_next_index_long(return_value, pctx->total_steps);
		add_next_index_long(return_value, pctx->progress);
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_exportchanges_updatestate)
{
	uint32_t result;
	BINARY state_bin;
	zval *pzresstream;
	ICS_EXPORT_CTX *pctx;
	STREAM_OBJECT *pstream;
	zval *pzresexportchanges;
	
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rr",
		&pzresexportchanges, &pzresstream) == FAILURE || NULL
		== pzresexportchanges || NULL == pzresstream) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
    ZEND_FETCH_RESOURCE(pctx, ICS_EXPORT_CTX*, &pzresexportchanges,
		-1, name_mapi_exportchanges, le_mapi_exportchanges);
    ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresstream, -1, name_stream, le_stream);
	result = zarafa_client_statesync(pctx->hsession,
						pctx->hobject, &state_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	stream_object_reset(pstream);
	stream_object_write(pstream, state_bin.pb, state_bin.cb);
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_exportchanges_getchangecount)
{
	zval *pzresource;
	ICS_EXPORT_CTX *pctx;
	
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r", &pzresource) == FAILURE || NULL == pzresource) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
    ZEND_FETCH_RESOURCE(pctx, ICS_EXPORT_CTX*, &pzresource,
		-1, name_mapi_exportchanges, le_mapi_exportchanges);
	if (0 == pctx->total_steps && 0 != pctx->b_changed) {
		RETVAL_LONG(1);
	} else {
		RETVAL_LONG(pctx->total_steps);
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importcontentschanges_config)
{
	long flags;
	uint32_t result;
	zval *pzresimport;
	zval *pzresstream;
	ICS_IMPORT_CTX *pctx;
	STREAM_OBJECT *pstream;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rrl",
		&pzresimport, &pzresstream, &flags) == FAILURE || NULL
		== pzresimport || NULL == pzresstream) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx, ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importcontentschanges, le_mapi_importcontentschanges);
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresstream, -1, name_stream, le_stream);
	result = zarafa_client_configimport(pctx->hsession,
					pctx->hobject, ICS_TYPE_CONTENTS,
					stream_object_get_content(pstream));
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importcontentschanges_updatestate)
{
	uint32_t result;
	BINARY state_bin;
	zval *pzresimport;
	zval *pzresstream;
	ICS_IMPORT_CTX *pctx;
	STREAM_OBJECT *pstream;
	
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r|r", &pzresimport, &pzresstream) == FAILURE ||
		NULL == pzresimport || NULL == pzresstream) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx, ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importcontentschanges, le_mapi_importcontentschanges);
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresstream, -1, name_stream, le_stream);
	result = zarafa_client_stateimport(pctx->hsession,
							pctx->hobject, &state_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	stream_object_reset(pstream);
	stream_object_write(pstream, state_bin.pb, state_bin.cb);
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importcontentschanges_importmessagechange)
{
	long flags;
	uint32_t result;
	uint32_t hobject;
	zval *pzresprops;
	zval *pzresimport;
	zval *pzresmessage;
	ICS_IMPORT_CTX *pctx;
	TPROPVAL_ARRAY propvals;
	MAPI_RESOURCE *presource;
	
	flags = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ralz", &pzresimport, &pzresprops, &flags,
		&pzresmessage) == FAILURE || NULL == pzresimport
		|| NULL == pzresprops || NULL == pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
    ZEND_FETCH_RESOURCE(pctx, ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importcontentschanges, le_mapi_importcontentschanges);
	if (!php_to_tpropval_array(pzresprops, &propvals TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
    result = zarafa_client_importmessage(pctx->hsession,
			pctx->hobject, flags, &propvals, &hobject);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	presource->type = MAPI_MESSAGE;
	presource->hsession = pctx->hsession;
	presource->hobject = hobject;
	ZEND_REGISTER_RESOURCE(pzresmessage, presource, le_mapi_message);
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importcontentschanges_importmessagedeletion)
{
	long flags;
	uint32_t result;
	zval *pzresimport;
	zval *pzresmessages;
	ICS_IMPORT_CTX *pctx;
	BINARY_ARRAY message_bins;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rla",
		&pzresimport, &flags, &pzresmessages) == FAILURE ||
		NULL == pzresimport || NULL == pzresmessages) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx,
		ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importcontentschanges,
		le_mapi_importcontentschanges);
	if (!php_to_binary_array(pzresmessages, &message_bins)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	if (SYNC_SOFT_DELETE & flags) {
		flags = 0;
	} else {
		flags = SYNC_DELETES_FLAG_HARDDELETE;
	}
	result = zarafa_client_importdeletion(
			pctx->hsession, pctx->hobject,
			flags, &message_bins);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importcontentschanges_importperuserreadstatechange)
{
	uint32_t result;
	zval *pzresimport;
	ICS_IMPORT_CTX *pctx;
	zval *pzresreadstates;
	STATE_ARRAY message_states;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra",
		&pzresimport, &pzresreadstates) == FAILURE || NULL ==
		pzresimport || NULL == pzresreadstates) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx, ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importcontentschanges, le_mapi_importcontentschanges);
	if (!php_to_state_array(pzresreadstates, &message_states TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_importreadstates(
				pctx->hsession, pctx->hobject,
				&message_states);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importcontentschanges_importmessagemove)
{
	MAPI_G(hr) = EC_NOT_IMPLEMENTED;
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importhierarchychanges_config)
{
	long flags;
	uint32_t result;
	zval *pzresimport;
	zval *pzresstream;
	ICS_IMPORT_CTX *pctx;
	STREAM_OBJECT *pstream;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rrl",
		&pzresimport, &pzresstream, &flags) == FAILURE || NULL
		== pzresimport || NULL == pzresstream) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx, ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importhierarchychanges, le_mapi_importhierarchychanges);
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresstream, -1, name_stream, le_stream);
	result = zarafa_client_configimport(pctx->hsession, pctx->hobject,
				ICS_TYPE_HIERARCHY, stream_object_get_content(pstream));
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importhierarchychanges_updatestate)
{
	uint32_t result;
	BINARY state_bin;
	zval *pzresimport;
	zval *pzresstream;
	ICS_IMPORT_CTX *pctx;
	STREAM_OBJECT *pstream;
	
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"r|r", &pzresimport, &pzresstream) == FAILURE ||
		NULL == pzresimport || NULL == pzresstream) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx,
		ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importhierarchychanges,
		le_mapi_importhierarchychanges);
	ZEND_FETCH_RESOURCE(pstream, STREAM_OBJECT*,
		&pzresstream, -1, name_stream, le_stream);
	result = zarafa_client_stateimport(pctx->hsession,
							pctx->hobject, &state_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	stream_object_reset(pstream);
	stream_object_write(pstream, state_bin.pb, state_bin.cb);
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importhierarchychanges_importfolderchange)
{
	uint32_t result;
	zval *pzresprops;
	zval *pzresimport;
	ICS_IMPORT_CTX *pctx;
	TPROPVAL_ARRAY propvals;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"ra", &pzresimport, &pzresprops) == FAILURE ||
		NULL == pzresimport || NULL == pzresprops) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx,
		ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importhierarchychanges,
		le_mapi_importhierarchychanges);
	if (!php_to_tpropval_array(pzresprops, &propvals TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_importfolder(
			pctx->hsession, pctx->hobject,
			&propvals);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_importhierarchychanges_importfolderdeletion)
{
	long flags;
	uint32_t result;
	zval *pzresimport;
	zval *pzresfolders;
	ICS_IMPORT_CTX *pctx;
	BINARY_ARRAY folder_bins;
	
	flags = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rla", &pzresimport, &flags, &pzresfolders) == FAILURE
		|| NULL == pzresimport || NULL == pzresfolders) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pctx, ICS_IMPORT_CTX*, &pzresimport, -1,
		name_mapi_importhierarchychanges, le_mapi_importhierarchychanges);
	if (!php_to_binary_array(pzresfolders, &folder_bins TSRMLS_CC)) {
		MAPI_G(hr) = EC_ERROR;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_importdeletion(
			pctx->hsession, pctx->hobject,
			flags, &folder_bins);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_wrap_importcontentschanges)
{
	zval *pzobject;
	ICS_IMPORT_CTX *pctx;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"o", &pzobject) == FAILURE || NULL == pzobject) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	pctx = st_malloc<ICS_IMPORT_CTX>();
	if (NULL == pctx) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	pctx->ics_type = ICS_TYPE_CONTENTS;
	pctx->hobject = 0;
	ZVAL_OBJ(&pctx->pztarget_obj, Z_OBJ_P(pzobject));
	Z_ADDREF(pctx->pztarget_obj);
#ifdef ZTS
	pctx->TSRMLS_C = TSRMLS_C;
#endif
	ZEND_REGISTER_RESOURCE(return_value,
		pctx, le_mapi_importcontentschanges);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_wrap_importhierarchychanges)
{
	zval *pzobject;
    ICS_IMPORT_CTX *pctx;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"o", &pzobject) == FAILURE || NULL == pzobject) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	pctx = st_malloc<ICS_IMPORT_CTX>();
	if (NULL == pctx) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	pctx->ics_type = ICS_TYPE_HIERARCHY;
	pctx->hobject = 0;
	ZVAL_OBJ(&pctx->pztarget_obj, Z_OBJ_P(pzobject));
	Z_ADDREF(pctx->pztarget_obj);
#ifdef ZTS
	pctx->TSRMLS_C = TSRMLS_C;
#endif
	ZEND_REGISTER_RESOURCE(return_value,
		pctx, le_mapi_importhierarchychanges);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_inetmapi_imtoinet)
{
	BINARY eml_bin;
	uint32_t result;
	zval *pzressession;
	zval *pzresmessage;
	zval *pzresoptions;
	zval *pzresaddrbook;
	STREAM_OBJECT *pstream;
	MAPI_RESOURCE *pmessage;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rrra", &pzressession, &pzresaddrbook, &pzresmessage,
		&pzresoptions) == FAILURE || NULL == pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresmessage, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_messagetorfc822(
		pmessage->hsession, pmessage->hobject,
		&eml_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	pstream = stream_object_create();
	if (NULL == pstream) {
		MAPI_G(hr) = EC_OUT_OF_MEMORY;
		goto THROW_EXCEPTION;
	}
	stream_object_write(pstream, eml_bin.pb, eml_bin.cb);
	stream_object_seek(pstream, STREAM_SEEK_SET, 0);
	ZEND_REGISTER_RESOURCE(return_value, pstream, le_stream);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_inetmapi_imtomapi)
{
	int cbstring;
	char *szstring;
	BINARY eml_bin;
	uint32_t result;
	zval *pzresstore;
	zval *pzressession;
	zval *pzresmessage;
	zval *pzresoptions;
	zval *pzresaddrbook;
	MAPI_RESOURCE *pmessage;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rrrrsa",
		&pzressession, &pzresstore, &pzresaddrbook, &pzresmessage,
		&szstring, &cbstring, &pzresoptions) == FAILURE || NULL ==
		pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresmessage, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	eml_bin.pb = reinterpret_cast<uint8_t *>(szstring);
	eml_bin.cb = cbstring;
	result = zarafa_client_rfc822tomessage(
		pmessage->hsession, pmessage->hobject,
		&eml_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}    

ZEND_FUNCTION(mapi_icaltomapi)
{
	int cbstring;
	char *szstring;
	BINARY ical_bin;
	uint32_t result;
	zval *pzresstore;
	zval *pzresmessage;
	zval *pzressession;
	zval *pzresaddrbook;
	MAPI_RESOURCE *pmessage;
	zend_bool b_norecipients;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rrrrsb",
		&pzressession, &pzresstore, &pzresaddrbook, &pzresmessage,
		&szstring, &cbstring, &b_norecipients) == FAILURE || NULL
		== pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresmessage, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ical_bin.pb = reinterpret_cast<uint8_t *>(szstring);
	ical_bin.cb = cbstring;
	result = zarafa_client_icaltomessage(
		pmessage->hsession, pmessage->hobject,
		&ical_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_mapitoical)
{
	BINARY ical_bin;
	uint32_t result;
	zval *pzressession;
	zval *pzresmessage;
	zval *pzresoptions;
	zval *pzresaddrbook;
	MAPI_RESOURCE *pmessage;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rrra", &pzressession, &pzresaddrbook, &pzresmessage,
		&pzresoptions) == FAILURE || NULL == pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;	
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresmessage, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_messagetoical(
		pmessage->hsession, pmessage->hobject,
		&ical_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_STRINGL(reinterpret_cast<const char *>(ical_bin.pb), ical_bin.cb);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_vcftomapi)
{
	int cbstring;
	char *szstring;
	BINARY vcf_bin;
	uint32_t result;
	zval *pzresstore;
	zval *pzressession;
	zval *pzresmessage;
	MAPI_RESOURCE *pmessage;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rrrs",
	    &pzressession, &pzresstore, &pzresmessage, &szstring,
	    &cbstring) == FAILURE || NULL == pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresmessage, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	vcf_bin.pb = reinterpret_cast<uint8_t *>(szstring);
	vcf_bin.cb = cbstring;
	result = zarafa_client_vcftomessage(
		pmessage->hsession, pmessage->hobject,
		&vcf_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_TRUE;
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_mapitovcf)
{
	BINARY vcf_bin;
	uint32_t result;
	zval *pzressession;
	zval *pzresmessage;
	zval *pzresoptions;
	zval *pzresaddrbook;
	MAPI_RESOURCE *pmessage;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rrra", &pzressession, &pzresaddrbook, &pzresmessage,
		&pzresoptions) == FAILURE || NULL == pzresmessage) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(pmessage, MAPI_RESOURCE*,
		&pzresmessage, -1, name_mapi_message, le_mapi_message);
	if (MAPI_MESSAGE != pmessage->type) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_messagetovcf(
		pmessage->hsession, pmessage->hobject,
		&vcf_bin);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;	
	}
	RETVAL_STRINGL(reinterpret_cast<const char *>(vcf_bin.pb), vcf_bin.cb);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_enable_exceptions)
{
	zend_string *clsname;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
	    "S", &clsname) == FAILURE) {
		RETVAL_FALSE;
		return;
	}
	auto ce = *reinterpret_cast<zend_class_entry **>(zend_hash_find(CG(class_table), clsname));
	if (ce != nullptr) {
		MAPI_G(exceptions_enabled) = 1;
		MAPI_G(exception_ce) = ce;
		RETVAL_TRUE;
	} else {
		RETVAL_FALSE;
	}
}

ZEND_FUNCTION(mapi_feature)
{
	int i;
	int cbfeature;
	const char *szfeature;
	static const char *const features[] =
		{"LOGONFLAGS", "NOTIFICATIONS",
		"INETMAPI_IMTOMAPI", "ST_ONLY_WHEN_OOF"};
	
	RETVAL_FALSE;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"s", &szfeature, &cbfeature) == FAILURE ||
		NULL == szfeature || 0 == cbfeature) {
		return;
	}
	for (i=0; i<sizeof(features)/sizeof(char*); i++) {
		if (0 == strcasecmp(features[i], szfeature)) {
			RETVAL_TRUE;
			return;
		}
	}
}

ZEND_FUNCTION(mapi_msgstore_abortsubmit)
{
	RETVAL_TRUE;
}

ZEND_FUNCTION(kc_session_save)
{
	zval *pzres;
	zval *pzoutstr;
	PUSH_CTX push_ctx;
	MAPI_RESOURCE *psession;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"rz", &pzres, &pzoutstr) == FAILURE || NULL ==
		pzres || NULL == pzoutstr) {
		RETVAL_LONG(EC_INVALID_PARAMETER);
		return;	
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzres, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		RETVAL_LONG(EC_INVALID_PARAMETER);
		return;	
	}
	if (!ext_pack_push_init(&push_ctx)) {
		RETVAL_LONG(EC_OUT_OF_MEMORY);
		return;	
	}
	ext_pack_push_guid(&push_ctx, &psession->hsession);
	ZVAL_STRINGL(pzoutstr, reinterpret_cast<const char *>(push_ctx.data), push_ctx.offset);
	ext_pack_push_free(&push_ctx);
	RETVAL_LONG(EC_SUCCESS);
}

ZEND_FUNCTION(kc_session_restore)
{
	zval *pzres;
	zval *pzdata;
	GUID hsession;
	BINARY data_bin;
	uint32_t result;
	PULL_CTX pull_ctx;
	MAPI_RESOURCE *presource;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"zz", &pzdata, &pzres) == FAILURE || NULL == pzdata
		|| NULL == pzres || Z_TYPE_P(pzdata) != IS_STRING) {
		RETVAL_LONG(EC_INVALID_PARAMETER);
		return;
	}
	data_bin.pb = reinterpret_cast<uint8_t *>(Z_STRVAL_P(pzdata));
	data_bin.cb = Z_STRLEN_P(pzdata);
	ext_pack_pull_init(&pull_ctx, data_bin.pb, data_bin.cb);
	if (!ext_pack_pull_guid(&pull_ctx, &hsession)) {
		RETVAL_LONG(EC_INVALID_PARAMETER);
		return;
	}
	result = zarafa_client_checksession(hsession);
	if (EC_SUCCESS != result) {
		RETVAL_LONG(result);
		return;
	}
	presource = st_malloc<MAPI_RESOURCE>();
	if (NULL == presource) {
		RETVAL_LONG(EC_OUT_OF_MEMORY);
		return;
	}
	presource->type = MAPI_SESSION;
	presource->hobject = 0;
	presource->hsession = hsession;
	ZEND_REGISTER_RESOURCE(pzres, presource, le_mapi_session);
	RETVAL_LONG(EC_SUCCESS);
}


ZEND_FUNCTION(nsp_getuserinfo)
{
	char *px500dn;
	BINARY entryid;
	char *username;
	uint32_t result;
	int username_len;
	char *pdisplay_name;
	uint32_t privilege_bits;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"s", &username, &username_len) == FAILURE) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;	
	}
	result = zarafa_client_uinfo(username, &entryid,
		&pdisplay_name, &px500dn, &privilege_bits);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	array_init(return_value);
	add_assoc_stringl(return_value, "userid", reinterpret_cast<const char *>(entryid.pb), entryid.cb);
	add_assoc_string(return_value, "username", username);
	add_assoc_string(return_value, "fullname", pdisplay_name);
	add_assoc_string(return_value, "essdn", px500dn);
	add_assoc_long(return_value, "privilege", privilege_bits);
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(nsp_setuserpasswd)
{
	char *username;
	uint32_t result;
	int username_len;
	char *old_passwd;
	char *new_passwd;
	int old_passwd_len;
	int new_passwd_len;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
		"sss", &username, &username_len, &old_passwd,
		&old_passwd_len, &new_passwd, &new_passwd_len)
		== FAILURE) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;	
	}
	result = zarafa_client_setpasswd(username, old_passwd, new_passwd);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	RETVAL_TRUE;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}

ZEND_FUNCTION(mapi_linkmessage)
{
	uint32_t result;
	zval *pzresource;
	BINARY search_entryid;
	BINARY message_entryid;
	MAPI_RESOURCE *psession;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|ss",
		&pzresource, &search_entryid.pb, &search_entryid.cb,
		&message_entryid.pb, &message_entryid.cb) == FAILURE
		|| NULL == pzresource || NULL == search_entryid.pb ||
		NULL == message_entryid.pb) {
		MAPI_G(hr) = EC_INVALID_PARAMETER;
		goto THROW_EXCEPTION;
	}
	ZEND_FETCH_RESOURCE(psession, MAPI_RESOURCE*,
		&pzresource, -1, name_mapi_session, le_mapi_session);
	if (MAPI_SESSION != psession->type) {
		MAPI_G(hr) = EC_INVALID_OBJECT;
		goto THROW_EXCEPTION;
	}
	result = zarafa_client_linkmessage(psession->hsession,
						search_entryid, message_entryid);
	if (EC_SUCCESS != result) {
		MAPI_G(hr) = result;
		goto THROW_EXCEPTION;
	}
	MAPI_G(hr) = EC_SUCCESS;
	return;
THROW_EXCEPTION:
	if (MAPI_G(exceptions_enabled)) {
		zend_throw_exception(MAPI_G(exception_ce),
			"MAPI error ", MAPI_G(hr) TSRMLS_CC);
	}
	RETVAL_FALSE;
}
