// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <cstdint>
#include <cstring>
#include <memory>
#include <libHX/string.h>
#include <gromox/mapidefs.h>
#include "container_object.h"
#include "zarafa_server.h"
#include "common_util.h"
#include <gromox/ext_buffer.hpp>
#include <gromox/tarray_set.hpp>
#include <gromox/mail_func.hpp>
#include <gromox/rop_util.hpp>
#include "ab_tree.h"
#include "exmdb_client.h"
#include "store_object.h"
#include "object_tree.h"
#include <gromox/propval.hpp>
#include <cstdio>

using namespace gromox;

std::unique_ptr<container_object>
container_object::create(uint8_t type, CONTAINER_ID id)
{
	std::unique_ptr<container_object> pcontainer;
	try {
		pcontainer.reset(new container_object);
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	pcontainer->type = type;
	pcontainer->id = id;
	return pcontainer;
}

void container_object::clear()
{
	auto pcontainer = this;
	if (CONTAINER_TYPE_ABTREE == pcontainer->type) {
		if (NULL == pcontainer->contents.pminid_array) {
			return;
		}
		if (NULL != pcontainer->contents.pminid_array->pl) {
			free(pcontainer->contents.pminid_array->pl);
		}
		free(pcontainer->contents.pminid_array);
		pcontainer->contents.pminid_array = NULL;
	} else {
		if (NULL != pcontainer->contents.prow_set) {
			tarray_set_free(pcontainer->contents.prow_set);
			pcontainer->contents.prow_set = NULL;
		}
	}
}

static BOOL container_object_match_contact_message(
	const TPROPVAL_ARRAY *ppropvals, const RESTRICTION *pfilter)
{
	switch (pfilter->rt) {
	case RES_AND:
		for (size_t i = 0; i < pfilter->andor->count; ++i)
			if (!container_object_match_contact_message(ppropvals, &pfilter->andor->pres[i]))
				return FALSE;
		return TRUE;
	case RES_OR:
		for (size_t i = 0; i < pfilter->andor->count; ++i)
			if (container_object_match_contact_message(ppropvals, &pfilter->andor->pres[i]))
				return TRUE;
		return FALSE;
	case RES_NOT:
		if (container_object_match_contact_message(ppropvals, &pfilter->xnot->res))
			return FALSE;
		return TRUE;
	case RES_CONTENT: {
		auto rcon = pfilter->cont;
		if (PROP_TYPE(rcon->proptag) != PT_UNICODE)
			return FALSE;
		if (PROP_TYPE(rcon->proptag) != PROP_TYPE(rcon->propval.proptag))
			return FALSE;
		auto pvalue = ppropvals->getval(rcon->proptag);
		if (NULL == pvalue) {
			return FALSE;	
		}
		switch (rcon->fuzzy_level & 0xFFFF) {
		case FL_FULLSTRING:
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strcasecmp(static_cast<char *>(rcon->propval.pvalue),
				    static_cast<char *>(pvalue)) == 0)
					return TRUE;
			} else {
				if (strcmp(static_cast<char *>(rcon->propval.pvalue),
				    static_cast<char *>(pvalue)) == 0)
					return TRUE;
			}
			return FALSE;
		case FL_SUBSTRING:
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strcasestr(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue)) != nullptr)
					return TRUE;
			} else {
				if (strstr(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue)) != nullptr)
					return TRUE;
			}
			return FALSE;
		case FL_PREFIX: {
			auto len = strlen(static_cast<char *>(rcon->propval.pvalue));
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strncasecmp(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue),
				    len) == 0)
					return TRUE;
			} else {
				if (strncmp(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue),
				    len) == 0)
					return TRUE;
			}
			return FALSE;
		}
		}
		return FALSE;
	}
	case RES_PROPERTY: {
		auto rprop = pfilter->prop;
		if (rprop->proptag == PROP_TAG_ANR) {
			auto pvalue = ppropvals->get<char>(PR_SMTP_ADDRESS);
			if (NULL != pvalue) {
				if (strcasestr(pvalue,
				    static_cast<char *>(rprop->propval.pvalue)) != nullptr)
					return TRUE;
			}
			pvalue = ppropvals->get<char>(PR_DISPLAY_NAME);
			if (NULL != pvalue) {
				if (strcasestr(pvalue,
				    static_cast<char *>(rprop->propval.pvalue)) != nullptr)
					return TRUE;
			}
			return FALSE;
		}
		auto pvalue = ppropvals->getval(rprop->proptag);
		if (NULL == pvalue) {
			return FALSE;
		}
		return propval_compare_relop(rprop->relop,
		       PROP_TYPE(rprop->proptag), pvalue, rprop->propval.pvalue);
	}
	case RES_BITMASK: {
		auto rbm = pfilter->bm;
		if (PROP_TYPE(rbm->proptag) != PT_LONG)
			return FALSE;
		auto pvalue = ppropvals->get<uint32_t>(rbm->proptag);
		if (NULL == pvalue) {
			return FALSE;
		}
		switch (rbm->bitmask_relop) {
		case BMR_EQZ:
			if (!(*pvalue & rbm->mask))
				return TRUE;
			break;
		case BMR_NEZ:
			if (*pvalue & rbm->mask)
				return TRUE;
			break;
		}
		return FALSE;
	}
	case RES_EXIST:
		return ppropvals->has(pfilter->exist->proptag) ? TRUE : false;
	default:
		return FALSE;
	}
	return false;
}

static BOOL container_object_get_pidlids(PROPTAG_ARRAY *pproptags)
{
	int i;
	uint8_t mapi_type;
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	PROPERTY_NAME propname_buff[9];
	
	auto pinfo = zarafa_server_get_info();
	auto handle = pinfo->ptree->get_store_handle(TRUE, pinfo->user_id);
	auto pstore = pinfo->ptree->get_object<store_object>(handle, &mapi_type);
	if (pstore == nullptr || mapi_type != ZMG_STORE)
		return FALSE;
	propnames.count = 9;
	propnames.ppropname = propname_buff;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[0].guid);
	propname_buff[0].kind = MNID_ID;
	propname_buff[0].lid = PidLidEmail1DisplayName;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[1].guid);
	propname_buff[1].kind = MNID_ID;
	propname_buff[1].lid = PidLidEmail1AddressType;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[2].guid);
	propname_buff[2].kind = MNID_ID;
	propname_buff[2].lid = PidLidEmail1EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[3].guid);
	propname_buff[3].kind = MNID_ID;
	propname_buff[3].lid = PidLidEmail2DisplayName;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[4].guid);
	propname_buff[4].kind = MNID_ID;
	propname_buff[4].lid = PidLidEmail2AddressType;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[5].guid);
	propname_buff[5].kind = MNID_ID;
	propname_buff[5].lid = PidLidEmail2EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[6].guid);
	propname_buff[6].kind = MNID_ID;
	propname_buff[6].lid = PidLidEmail3DisplayName;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[7].guid);
	propname_buff[7].kind = MNID_ID;
	propname_buff[7].lid = PidLidEmail3AddressType;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[8].guid);
	propname_buff[8].kind = MNID_ID;
	propname_buff[8].lid = PidLidEmail3EmailAddress;
	if (!pstore->get_named_propids(false, &propnames, &propids) ||
	    propids.count != 9)
		return FALSE;
	for (i=0; i<9; i++) {
		pproptags->pproptag[i] = PROP_TAG(PT_UNICODE, propids.ppropid[i]);
	}
	pproptags->count = 9;
	return TRUE;
}

static BINARY* container_object_folder_to_addressbook_entryid(
	BOOL b_private, int db_id, uint64_t folder_id)
{
	BINARY *pbin;
	char x500dn[128];
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	uint8_t type = b_private ? LOC_TYPE_PRIVATE_FOLDER : LOC_TYPE_PUBLIC_FOLDER;
	
	memcpy(x500dn, "/exmdb=", 7);
	common_util_exmdb_locinfo_to_string(
		type, db_id, folder_id, x500dn + 7);
	tmp_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
								tmp_entryid.provider_uid);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_CONTAINER;
	tmp_entryid.px500dn = x500dn;
	pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(256);
	if (pbin->pv == nullptr ||
	    !ext_push.init(pbin->pb, 256, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;
	pbin->cb = ext_push.m_offset;
	return pbin;
}

static BINARY* container_object_message_to_addressbook_entryid(
	BOOL b_private, int db_id, uint64_t message_id, int num)
{
	int len;
	BINARY *pbin;
	char x500dn[128];
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	uint8_t type = b_private ? LOC_TYPE_PRIVATE_MESSAGE : LOC_TYPE_PUBLIC_MESSAGE;
	
	memcpy(x500dn, "/exmdb=", 7);
	common_util_exmdb_locinfo_to_string(
		type, db_id, message_id, x500dn + 7);
	len = strlen(x500dn);
	sprintf(x500dn + len, ":%d", num);
	tmp_entryid.flags = 0;
	memcpy(tmp_entryid.provider_uid,
		common_util_get_muidzcsab(), 16);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_REMOTE_USER;
	tmp_entryid.px500dn = x500dn;
	pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(256);
	if (pbin->pv == nullptr ||
	    !ext_push.init(pbin->pb, 256, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BOOL container_object::load_user_table(const RESTRICTION *prestriction)
{
	auto pcontainer = this;
	BINARY tmp_bin;
	uint32_t tmp_int;
	uint32_t row_num;
	uint32_t table_id;
	uint8_t mapi_type;
	char username[UADDR_SIZE];
	TARRAY_SET tmp_set;
	PROPTAG_ARRAY proptags;
	LONG_ARRAY minid_array;
	BINARY *pparent_entryid = nullptr;
	LONG_ARRAY *pminid_array;
	TPROPVAL_ARRAY *ppropvals;
	uint32_t proptag_buff[25];
	static constexpr uint32_t tmp_proptags[] = {
		PR_NICKNAME, PR_SURNAME, PR_GIVEN_NAME, PR_MIDDLE_NAME,
		PR_TITLE, PR_PRIMARY_TELEPHONE_NUMBER,
		PR_MOBILE_TELEPHONE_NUMBER, PR_HOME_ADDRESS_STREET, PR_COMMENT,
		PR_COMPANY_NAME, PR_DEPARTMENT_NAME, PR_OFFICE_LOCATION,
		PR_CREATION_TIME
	};
	
	if (CONTAINER_TYPE_ABTREE == pcontainer->type) {
		if (NULL == prestriction ||
			NULL != pcontainer->contents.pminid_array) {
			return TRUE;
		}
		auto pbase = ab_tree_get_base(pcontainer->id.abtree_id.base_id);
		if (pbase == nullptr)
			return FALSE;
		auto pinfo = zarafa_server_get_info();
		if (!ab_tree_match_minids(pbase.get(), pcontainer->id.abtree_id.minid,
			pinfo->cpid, prestriction, &minid_array)) {
			return FALSE;	
		}
		pbase.reset();
		pminid_array = me_alloc<LONG_ARRAY>();
		if (NULL == pminid_array) {
			return FALSE;
		}
		pcontainer->contents.pminid_array = pminid_array;
		pminid_array->count = minid_array.count;
		if (0 == minid_array.count) {
			pminid_array->pl = NULL;
			return TRUE;
		}
		pminid_array->pl = me_alloc<uint32_t>(minid_array.count);
		if (NULL == pminid_array->pl) {
			free(pcontainer->contents.pminid_array);
			pcontainer->contents.pminid_array = NULL;
			return FALSE;
		}
		memcpy(pminid_array->pl, minid_array.pl,
			sizeof(uint32_t)*minid_array.count);
		return TRUE;
	}
	if (NULL != pcontainer->contents.prow_set) {
		return TRUE;
	}
	auto pinfo = zarafa_server_get_info();
	if (!exmdb_client::load_content_table(pinfo->get_maildir(),
	    pinfo->cpid, pcontainer->id.exmdb_id.folder_id, nullptr, 0,
	    nullptr, nullptr, &table_id, &row_num))
		return FALSE;
	if (row_num > 0) {
		proptags.pproptag = proptag_buff;
		if (FALSE == container_object_get_pidlids(&proptags)) {
			return FALSE;
		}
		proptags.pproptag[proptags.count++] = PR_DISPLAY_NAME;
		proptags.pproptag[proptags.count++] = PR_NICKNAME;
		proptags.pproptag[proptags.count++] = PR_TITLE;
		proptags.pproptag[proptags.count++] = PR_SURNAME;
		proptags.pproptag[proptags.count++] = PR_GIVEN_NAME;
		proptags.pproptag[proptags.count++] = PR_MIDDLE_NAME;
		proptags.pproptag[proptags.count++] = PR_PRIMARY_TELEPHONE_NUMBER;
		proptags.pproptag[proptags.count++] = PR_MOBILE_TELEPHONE_NUMBER;
		proptags.pproptag[proptags.count++] = PR_HOME_ADDRESS_STREET;
		proptags.pproptag[proptags.count++] = PR_COMMENT;
		proptags.pproptag[proptags.count++] = PR_COMPANY_NAME;
		proptags.pproptag[proptags.count++] = PR_DEPARTMENT_NAME;
		proptags.pproptag[proptags.count++] = PR_OFFICE_LOCATION;
		proptags.pproptag[proptags.count++] = PR_CREATION_TIME;
		proptags.pproptag[proptags.count++] = PROP_TAG_MID;
		if (!exmdb_client::query_table(pinfo->get_maildir(), nullptr,
		    pinfo->cpid, table_id, &proptags, 0, row_num, &tmp_set))
			return FALSE;
		pparent_entryid = container_object_folder_to_addressbook_entryid(
				TRUE, pinfo->user_id, pcontainer->id.exmdb_id.folder_id);
		if (NULL == pparent_entryid) {
			return FALSE;
		}
		auto handle = pinfo->ptree->get_store_handle(TRUE, pinfo->user_id);
		auto pstore = pinfo->ptree->get_object<store_object>(handle, &mapi_type);
		if (pstore == nullptr || mapi_type != ZMG_STORE)
			return FALSE;
	} else {
		tmp_set.count = 0;
	}
	exmdb_client::unload_table(pinfo->get_maildir(), table_id);
	pcontainer->contents.prow_set = tarray_set_init();
	if (NULL == pcontainer->contents.prow_set) {
		return FALSE;
	}
	for (size_t i = 0; i < tmp_set.count; ++i) {
		for (unsigned int j = 0; j < 3; ++j) {
			auto pdisplayname = tmp_set.pparray[i]->get<char>(proptags.pproptag[3*j]);
			if (NULL == pdisplayname) {
				pdisplayname = tmp_set.pparray[i]->get<char>(PR_DISPLAY_NAME);
			}
			auto paddress_type = tmp_set.pparray[i]->get<char>(proptags.pproptag[3*j+1]);
			auto paddress = tmp_set.pparray[i]->get<char>(proptags.pproptag[3*j+2]);
			if (NULL == paddress || NULL == paddress_type) {
				continue;
			}
			if (0 == strcasecmp(paddress_type, "EX")) {
				if (!common_util_essdn_to_username(paddress,
				    username, GX_ARRAY_SIZE(username)))
					continue;
			} else if (0 == strcasecmp(paddress_type, "SMTP")) {
				gx_strlcpy(username, paddress, GX_ARRAY_SIZE(username));
			} else {
				continue;
			}
			ppropvals = tpropval_array_init();
			if (NULL == ppropvals) {
				return FALSE;
			}
			if (ppropvals->set(PR_SMTP_ADDRESS, username) != 0 ||
			    ppropvals->set(PR_ACCOUNT, username) != 0 ||
			    ppropvals->set(PR_ADDRTYPE, "SMTP") != 0 ||
			    ppropvals->set(PR_EMAIL_ADDRESS, username) != 0) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			if (NULL != pdisplayname) {
				if (ppropvals->set(PR_DISPLAY_NAME, pdisplayname) != 0 ||
				    ppropvals->set(PR_TRANSMITABLE_DISPLAY_NAME, pdisplayname) != 0 ||
				    ppropvals->set(PR_EMS_AB_DISPLAY_NAME_PRINTABLE, pdisplayname) != 0) {
					tpropval_array_free(ppropvals);
					return FALSE;
				}
			}
			for (size_t k = 0; k < GX_ARRAY_SIZE(tmp_proptags); ++k) {
				uint32_t tag = tmp_proptags[k];
				auto newval = tmp_set.pparray[i]->getval(tag);
				if (newval == nullptr)
					continue;
				if (ppropvals->set(tag, newval) != 0) {
					tpropval_array_free(ppropvals);
					return FALSE;
				}
			}
			if (ppropvals->set(PR_PARENT_ENTRYID, pparent_entryid) != 0) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			auto pvalue = tmp_set.pparray[i]->getval(PROP_TAG_MID);
			if (NULL == pvalue) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			pvalue = container_object_message_to_addressbook_entryid(
								TRUE, pinfo->user_id, *(uint64_t*)pvalue, j);
			if (pvalue == nullptr ||
			    ppropvals->set(PR_ENTRYID, pvalue) != 0 ||
			    ppropvals->set(PR_RECORD_KEY, pvalue) != 0 ||
			    ppropvals->set(PR_TEMPLATEID, pvalue) != 0 ||
			    ppropvals->set(PR_ORIGINAL_ENTRYID, pvalue) != 0) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			tmp_bin.cb = 16;
			tmp_bin.pb = deconst(common_util_get_muidzcsab());
			if (ppropvals->set(PROP_TAG_ABPROVIDERID, &tmp_bin) != 0) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			tmp_int = MAPI_MAILUSER;
			if (ppropvals->set(PR_OBJECT_TYPE, &tmp_int) != 0) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			tmp_int = DT_MAILUSER;
			if (ppropvals->set(PR_DISPLAY_TYPE, &tmp_int) != 0 ||
			    ppropvals->set(PR_DISPLAY_TYPE_EX, &tmp_int) != 0) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
			if (NULL != prestriction && FALSE ==
				container_object_match_contact_message(
				ppropvals, prestriction)) {
				tpropval_array_free(ppropvals);
				continue;
			}
			if (!tarray_set_append_internal(pcontainer->contents.prow_set, ppropvals)) {
				tpropval_array_free(ppropvals);
				return FALSE;
			}
		}
	}
	return TRUE;
}

BOOL container_object_fetch_special_property(
	uint8_t special_type, uint32_t proptag, void **ppvalue)
{
	void *pvalue;
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID ab_entryid;
	
	switch (proptag) {
	case PROP_TAG_ABPROVIDERID: {
		auto bv = cu_alloc<BINARY>();
		if (bv == nullptr)
			return FALSE;
		*ppvalue = bv;
		bv->cb = 16;
		bv->pb = deconst(common_util_get_muidecsab());
		return TRUE;
	}
	case PR_ENTRYID: {
		pvalue = cu_alloc<BINARY>();
		if (NULL == pvalue) {
			return FALSE;
		}
		auto bv = static_cast<BINARY *>(pvalue);
		ab_entryid.flags = 0;
		rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
									ab_entryid.provider_uid);
		ab_entryid.version = 1;
		ab_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_CONTAINER;
		ab_entryid.px500dn = special_type == SPECIAL_CONTAINER_GAL ?
		                     deconst("") : deconst("/");
		bv->pv = common_util_alloc(128);
		if (bv->pv == nullptr ||
		    !ext_push.init(static_cast<BINARY *>(pvalue)->pb, 128, 0) ||
		    ext_push.p_abk_eid(&ab_entryid) != EXT_ERR_SUCCESS)
			return FALSE;
		bv->cb = ext_push.m_offset;
		*ppvalue = pvalue;
		return TRUE;
	}
	case PROP_TAG_CONTAINERFLAGS:
		pvalue = cu_alloc<uint32_t>();
		if (NULL == pvalue) {
			return FALSE;
		}
		*(uint32_t*)pvalue = AB_RECIPIENTS |
			AB_SUBCONTAINERS | AB_UNMODIFIABLE;
		*ppvalue = pvalue;
		return TRUE;
	case PROP_TAG_DEPTH:
		pvalue = cu_alloc<uint32_t>();
		if (NULL == pvalue) {
			return FALSE;
		}
		*(uint32_t*)pvalue = 0;
		*ppvalue = pvalue;
		return TRUE;
	case PR_DISPLAY_NAME:
		*ppvalue = special_type == SPECIAL_CONTAINER_GAL ?
		           deconst("Global Address List") :
		           deconst("Gromox Contact Folders");
		return TRUE;
	case PR_EMS_AB_IS_MASTER:
		pvalue = cu_alloc<uint8_t>();
		if (NULL == pvalue) {
			return FALSE;
		}
		*(uint8_t*)pvalue = 0;
		*ppvalue = pvalue;
		return TRUE;
	}
	*ppvalue = NULL;
	return TRUE;
}

static BOOL container_object_fetch_special_properties(
	uint8_t special_type, const PROPTAG_ARRAY *pproptags,
	TPROPVAL_ARRAY *ppropvals)
{
	int i;
	void *pvalue;
	
	ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	if (NULL == ppropvals->ppropval) {
		return FALSE;
	}
	ppropvals->count = 0;
	for (i=0; i<pproptags->count; i++) {
		if (FALSE == container_object_fetch_special_property(
			special_type, pproptags->pproptag[i], &pvalue)) {
			return FALSE;	
		}
		if (NULL == pvalue) {
			continue;
		}
		ppropvals->ppropval[ppropvals->count].proptag =
									pproptags->pproptag[i];
		ppropvals->ppropval[ppropvals->count].pvalue = pvalue;
		ppropvals->count ++;
	}
	return TRUE;
}

static BOOL container_object_fetch_folder_properties(
	const TPROPVAL_ARRAY *ppropvals, const PROPTAG_ARRAY *pproptags,
	TPROPVAL_ARRAY *pout_propvals)
{
	int i;
	int count;
	
	auto pvfid = ppropvals->get<uint64_t>(PROP_TAG_FOLDERID);
	if (pvfid == nullptr)
		return FALSE;
	auto folder_id = *pvfid;
	pout_propvals->count = 0;
	pout_propvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	if (NULL == pout_propvals->ppropval) {
		return FALSE;
	}
	for (i=0; i<pproptags->count; i++) {
		pout_propvals->ppropval[pout_propvals->count].proptag =
										pproptags->pproptag[i];
		switch (pproptags->pproptag[i]) {
		case PROP_TAG_ABPROVIDERID: {
			auto bv = cu_alloc<BINARY>();
			if (bv == nullptr)
				return FALSE;
			pout_propvals->ppropval[pout_propvals->count++].pvalue = bv;
			bv->cb = 16;
			bv->pb = deconst(common_util_get_muidzcsab());
			break;
		}
		case PR_ENTRYID:
		case PR_PARENT_ENTRYID: {
			auto pinfo = zarafa_server_get_info();
			void *pvalue = nullptr;
			if (pproptags->pproptag[i] == PR_PARENT_ENTRYID) {
				if (folder_id == rop_util_make_eid_ex(
					1, PRIVATE_FID_CONTACTS)) {
					if (!container_object_fetch_special_property(SPECIAL_CONTAINER_PROVIDER,
					    PR_ENTRYID, &pvalue))
						return FALSE;	
				} else {
					pvalue = ppropvals->getval(PROP_TAG_PARENTFOLDERID);
					if (NULL == pvalue) {
						return FALSE;
					}
					pvalue = container_object_folder_to_addressbook_entryid(
									TRUE, pinfo->user_id, *(uint64_t*)pvalue);
				}
			} else {
				pvalue = container_object_folder_to_addressbook_entryid(
										TRUE, pinfo->user_id, folder_id);
			}
			if (NULL == pvalue) {
				return FALSE;
			}
			pout_propvals->ppropval[pout_propvals->count].pvalue = pvalue;
			pout_propvals->count ++;
			break;
		}
		case PROP_TAG_CONTAINERFLAGS: {
			auto pvalue = ppropvals->get<uint32_t>(PROP_TAG_SUBFOLDERS);
			BOOL b_sub = pvalue == nullptr || *pvalue == 0 ? false : TRUE;
			pvalue = cu_alloc<uint32_t>();
			if (NULL == pvalue) {
				return FALSE;
			}
			*pvalue = b_sub ?
				AB_RECIPIENTS | AB_UNMODIFIABLE :
				AB_RECIPIENTS | AB_SUBCONTAINERS | AB_UNMODIFIABLE;
			pout_propvals->ppropval[pout_propvals->count].pvalue = pvalue;
			pout_propvals->count ++;
			break;
		}
		case PROP_TAG_DEPTH: {
			auto pc = ppropvals->get<const char>(PROP_TAG_FOLDERPATHNAME);
			if (pc == nullptr)
				return FALSE;
			count = 0;
			for (; *pc != '\0'; ++pc)
				if (*pc == '\\')
					count ++;
			if (count < 3) {
				return FALSE;
			}
			count -= 2;
			auto pvalue = cu_alloc<uint32_t>();
			if (NULL == pvalue) {
				return FALSE;
			}
			*pvalue = count;
			pout_propvals->ppropval[pout_propvals->count].pvalue = pvalue;
			pout_propvals->count ++;
			break;
		}
		case PR_DISPLAY_NAME: {
			auto pvalue = ppropvals->get<char>(PR_DISPLAY_NAME);
			if (NULL == pvalue) {
				return FALSE;
			}
			pout_propvals->ppropval[pout_propvals->count].pvalue = pvalue;
			pout_propvals->count ++;
			break;
		}
		case PR_EMS_AB_IS_MASTER: {
			auto pvalue = cu_alloc<uint8_t>();
			if (NULL == pvalue) {
				return FALSE;
			}
			*pvalue = 0;
			pout_propvals->ppropval[pout_propvals->count].pvalue = pvalue;
			pout_propvals->count ++;
			break;
		}
		}
	}
	return TRUE;
}

static const PROPTAG_ARRAY* container_object_get_folder_proptags()
{
	static const uint32_t proptag_buff[] = {
					PROP_TAG_FOLDERID,
					PROP_TAG_SUBFOLDERS,
					PR_DISPLAY_NAME,
					PR_CONTAINER_CLASS,
					PROP_TAG_FOLDERPATHNAME,
					PROP_TAG_PARENTFOLDERID,
					PR_ATTR_HIDDEN};
	static const PROPTAG_ARRAY proptags = {.count = 7, .pproptag = (uint32_t *)proptag_buff};
	return &proptags;
}

BOOL container_object::get_properties(const PROPTAG_ARRAY *pproptags,
    TPROPVAL_ARRAY *ppropvals)
{
	auto pcontainer = this;
	SIMPLE_TREE_NODE *pnode;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (CONTAINER_TYPE_ABTREE == pcontainer->type) {
		if (0 == pcontainer->id.abtree_id.minid) {
			return container_object_fetch_special_properties(
				SPECIAL_CONTAINER_PROVIDER, pproptags, ppropvals);
		}
		auto pbase = ab_tree_get_base(pcontainer->id.abtree_id.base_id);
		if (pbase == nullptr)
			return FALSE;
		pnode = ab_tree_minid_to_node(pbase.get(),
			pcontainer->id.abtree_id.minid);
		if (NULL == pnode) {
			ppropvals->count = 0;
			return TRUE;
		}
		if (FALSE == ab_tree_fetch_node_properties(
			pnode, pproptags, ppropvals)) {
			return FALSE;
		}
		return TRUE;
	} else {
		auto pinfo = zarafa_server_get_info();
		if (!exmdb_client::get_folder_properties(pinfo->get_maildir(),
		    pinfo->cpid, pcontainer->id.exmdb_id.folder_id,
		    container_object_get_folder_proptags(), &tmp_propvals))
			return FALSE;
		return container_object_fetch_folder_properties(
					&tmp_propvals, pproptags, ppropvals);
	}
}

BOOL container_object::get_container_table_num(BOOL b_depth, uint32_t *pnum)
{
	auto pcontainer = this;
	TARRAY_SET tmp_set;
	PROPTAG_ARRAY proptags;
	
	proptags.count = 0;
	proptags.pproptag = NULL;
	if (!pcontainer->query_container_table(&proptags, b_depth, 0,
	    0x7FFFFFFF, &tmp_set))
		return FALSE;	
	*pnum = tmp_set.count;
	return TRUE;
}

void container_object_get_container_table_all_proptags(
	PROPTAG_ARRAY *pproptags)
{
	static const uint32_t proptag_buff[] = {
		PR_ENTRYID,
		PROP_TAG_CONTAINERFLAGS,
		PROP_TAG_DEPTH,
		PR_INSTANCE_KEY,
		PR_EMS_AB_CONTAINERID,
		PR_DISPLAY_NAME,
		PR_EMS_AB_IS_MASTER,
		PR_EMS_AB_PARENT_ENTRYID,
		PROP_TAG_ABPROVIDERID
	};
	
	pproptags->count = 7;
	pproptags->pproptag = deconst(proptag_buff);
}

static BOOL container_object_get_specialtables_from_node(
	SIMPLE_TREE_NODE *pnode, const PROPTAG_ARRAY *pproptags,
	BOOL b_depth, TARRAY_SET *pset)
{
	TPROPVAL_ARRAY **pparray;
	auto count = strange_roundup(pset->count, SR_GROW_TPROPVAL_ARRAY);
	if (pset->count + 1 >= count) {
		count += SR_GROW_TPROPVAL_ARRAY;
		pparray = cu_alloc<TPROPVAL_ARRAY *>(count);
		if (NULL == pparray) {
			return FALSE;
		}
		memcpy(pparray, pset->pparray,
			pset->count*sizeof(TPROPVAL_ARRAY*));
		pset->pparray = pparray;
	}
	pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
	if (NULL == pset->pparray[pset->count]) {
		return FALSE;
	}
	if (FALSE == ab_tree_fetch_node_properties(
		pnode, pproptags, pset->pparray[pset->count])) {
		return FALSE;	
	}
	pset->count ++;
	if (TRUE == b_depth && TRUE == ab_tree_has_child(pnode)) {
		pnode = simple_tree_node_get_child(pnode);
		do {
			if (ab_tree_get_node_type(pnode) < 0x80) {
				continue;
			}
			if (FALSE == container_object_get_specialtables_from_node(
				pnode, pproptags, TRUE, pset)) {
				return FALSE;	
			}
		} while ((pnode = simple_tree_node_get_sibling(pnode)) != nullptr);
	}
	return TRUE;
}

static BOOL container_object_query_folder_hierarchy(
	uint64_t folder_id, const PROPTAG_ARRAY *pproptags,
	BOOL b_depth, TARRAY_SET *pset)
{
	uint32_t row_num;
	uint32_t table_id;
	TARRAY_SET tmp_set;
	TPROPVAL_ARRAY **pparray;
	
	auto pinfo = zarafa_server_get_info();
	if (!exmdb_client::load_hierarchy_table(pinfo->get_maildir(),
	    folder_id, nullptr, TABLE_FLAG_DEPTH, nullptr, &table_id, &row_num))
		return FALSE;
	if (row_num == 0)
		tmp_set.count = 0;
	else if (!exmdb_client::query_table(pinfo->get_maildir(), nullptr,
	    pinfo->cpid, table_id, container_object_get_folder_proptags(),
	    0, row_num, &tmp_set))
		return FALSE;
	exmdb_client::unload_table(pinfo->get_maildir(), table_id);
	for (size_t i = 0; i < tmp_set.count; ++i) {
		auto pvalue = tmp_set.pparray[i]->getval(PR_ATTR_HIDDEN);
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			continue;
		}
		pvalue = tmp_set.pparray[i]->getval(PR_CONTAINER_CLASS);
		if (pvalue == nullptr || strcasecmp(static_cast<char *>(pvalue), "IPF.Contact") != 0)
			continue;
		auto count = strange_roundup(pset->count, SR_GROW_TPROPVAL_ARRAY);
		if (pset->count + 1 >= count) {
			count += SR_GROW_TPROPVAL_ARRAY;
			pparray = cu_alloc<TPROPVAL_ARRAY *>(count);
			if (NULL == pparray) {
				return FALSE;
			}
			memcpy(pparray, pset->pparray,
				pset->count*sizeof(TPROPVAL_ARRAY*));
			pset->pparray = pparray;
		}
		pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == pset->pparray[pset->count]) {
			return FALSE;
		}
		if (FALSE == container_object_fetch_folder_properties(
			tmp_set.pparray[i], pproptags, pset->pparray[pset->count])) {
			return FALSE;	
		}
		pset->count ++;
	}
	return TRUE;
}

BOOL container_object::query_container_table(const PROPTAG_ARRAY *pproptags,
	BOOL b_depth, uint32_t start_pos, int32_t row_needed,
	TARRAY_SET *pset)
{
	auto pcontainer = this;
	TARRAY_SET tmp_set;
	DOMAIN_NODE *pdnode;
	SINGLE_LIST_NODE *psnode;
	SIMPLE_TREE_NODE *ptnode;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (0 == row_needed) {
		pset->count = 0;
		pset->pparray = NULL;
		return TRUE;
	}
	tmp_set.count = 0;
	tmp_set.pparray = cu_alloc<TPROPVAL_ARRAY *>(100);
	if (NULL == tmp_set.pparray) {
		return FALSE;
	}
	if (CONTAINER_TYPE_FOLDER == pcontainer->type) {
		if (FALSE == container_object_query_folder_hierarchy(
			pcontainer->id.exmdb_id.folder_id, pproptags,
			b_depth, &tmp_set)) {
			return FALSE;	
		}
	} else {
		auto pbase = ab_tree_get_base(pcontainer->id.abtree_id.base_id);
		if (pbase == nullptr)
			return FALSE;
		if (0xFFFFFFFF == pcontainer->id.abtree_id.minid) {
			tmp_set.pparray[tmp_set.count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == tmp_set.pparray[tmp_set.count]) {
				return FALSE;
			}
			if (FALSE == container_object_fetch_special_properties(
				SPECIAL_CONTAINER_GAL, pproptags,
				tmp_set.pparray[tmp_set.count])) {
				return FALSE;
			}
			tmp_set.count ++;
			tmp_set.pparray[tmp_set.count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == tmp_set.pparray[tmp_set.count]) {
				return FALSE;
			}
			if (FALSE == container_object_fetch_special_properties(
				SPECIAL_CONTAINER_PROVIDER, pproptags,
				tmp_set.pparray[tmp_set.count])) {
				return FALSE;
			}
			tmp_set.count ++;
			tmp_set.pparray[tmp_set.count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == tmp_set.pparray[tmp_set.count]) {
				return FALSE;
			}
			auto pinfo = zarafa_server_get_info();
			if (!exmdb_client::get_folder_properties(pinfo->get_maildir(),
				pinfo->cpid, rop_util_make_eid_ex(1, PRIVATE_FID_CONTACTS),
				container_object_get_folder_proptags(), &tmp_propvals)) {
				return FALSE;
			}
			if (FALSE == container_object_fetch_folder_properties(
				&tmp_propvals, pproptags, tmp_set.pparray[tmp_set.count])) {
				return FALSE;
			}
			tmp_set.count ++;
			if (b_depth && !container_object_query_folder_hierarchy(
			    rop_util_make_eid_ex(1, PRIVATE_FID_CONTACTS),
			    pproptags, TRUE, &tmp_set))
				return FALSE;
			for (psnode=single_list_get_head(&pbase->list); NULL!=psnode;
				psnode=single_list_get_after(&pbase->list, psnode)) {
				pdnode = (DOMAIN_NODE*)psnode->pdata;
				ptnode = simple_tree_get_root(&pdnode->tree);
				if (FALSE == container_object_get_specialtables_from_node(
					ptnode, pproptags, b_depth, &tmp_set)) {
					return FALSE;
				}
			}
		} else {
			ptnode = ab_tree_minid_to_node(pbase.get(),
				pcontainer->id.abtree_id.minid);
			if (NULL == ptnode) {
				pset->count = 0;
				pset->pparray = NULL;
				return TRUE;
			}
			if (NULL != (ptnode = simple_tree_node_get_child(ptnode))) {
				do {
					if (ab_tree_get_node_type(ptnode) < 0x80) {
						continue;
					}
					if (FALSE == container_object_get_specialtables_from_node(
						ptnode, pproptags, b_depth, &tmp_set)) {
						return FALSE;	
					}
				} while ((ptnode = simple_tree_node_get_sibling(ptnode)) != nullptr);
			}
		}
	}
	pset->count = 0;
	pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(tmp_set.count);
	if (NULL == pset->pparray) {
		return FALSE;
	}
	ssize_t end_pos = start_pos + row_needed;
	if (row_needed > 0) {
		if (end_pos > tmp_set.count) {
			end_pos = tmp_set.count;
		}
		for (ssize_t i = start_pos; i < end_pos; ++i) {
			pset->pparray[pset->count] = tmp_set.pparray[i];
			pset->count ++;
		}
	} else {
		if (end_pos < -1) {
			end_pos = -1;
		}
		for (ssize_t i = start_pos; i > end_pos; --i) {
			pset->pparray[pset->count] = tmp_set.pparray[i];
			pset->count ++;
		}
	}
	return TRUE;
}

BOOL container_object::get_user_table_num(uint32_t *pnum)
{
	auto pcontainer = this;
	SIMPLE_TREE_NODE *pnode;
	
	if (CONTAINER_TYPE_ABTREE == pcontainer->type) {
		if (NULL != pcontainer->contents.pminid_array) {
			*pnum = pcontainer->contents.pminid_array->count;
			return TRUE;
		}
		auto pbase = ab_tree_get_base(pcontainer->id.abtree_id.base_id);
		if (pbase == nullptr)
			return FALSE;
		*pnum = 0;
		if (0xFFFFFFFF == pcontainer->id.abtree_id.minid) {
			*pnum = single_list_get_nodes_num(&pbase->gal_list);
		} else if (0 == pcontainer->id.abtree_id.minid) {
			*pnum = 0;
		} else {
			pnode = ab_tree_minid_to_node(pbase.get(),
				pcontainer->id.abtree_id.minid);
			if (NULL == pnode || NULL == (pnode =
				simple_tree_node_get_child(pnode))) {
				return TRUE;
			}
			do {
				if (ab_tree_get_node_type(pnode) > 0x80) {
					continue;
				}
				(*pnum) ++;
			} while ((pnode = simple_tree_node_get_sibling(pnode)) != nullptr);
		}
	} else {
		if (NULL == pcontainer->contents.prow_set) {
			if (!pcontainer->load_user_table(nullptr))
				return FALSE;	
		}
		*pnum = pcontainer->contents.prow_set != nullptr ?
		        pcontainer->contents.prow_set->count : 0;
	}
	return TRUE;
}

void container_object_get_user_table_all_proptags(
	PROPTAG_ARRAY *pproptags)
{
	static const uint32_t proptag_buff[] = {
		PR_DISPLAY_NAME,
		PR_COMMENT,
		PR_COMPANY_NAME,
		PR_DEPARTMENT_NAME,
		PR_OFFICE_LOCATION,
		PR_ADDRTYPE,
		PR_SMTP_ADDRESS,
		PR_EMAIL_ADDRESS,
		PR_EMS_AB_DISPLAY_NAME_PRINTABLE,
		PR_ACCOUNT,
		PR_TRANSMITABLE_DISPLAY_NAME,
		PR_EMS_AB_PROXY_ADDRESSES,
		PR_OBJECT_TYPE,
		PR_DISPLAY_TYPE,
		PR_DISPLAY_TYPE_EX,
		PR_ENTRYID,
		PR_RECORD_KEY,
		PR_ORIGINAL_ENTRYID,
		PR_SEARCH_KEY,
		PR_INSTANCE_KEY,
		PR_MAPPING_SIGNATURE,
		PR_SEND_RICH_INFO,
		PR_TEMPLATEID,
		PR_EMS_AB_OBJECT_GUID,
		PR_CREATION_TIME,
		PROP_TAG_THUMBNAILPHOTO
	};
	pproptags->count = 34;
	pproptags->pproptag = deconst(proptag_buff);
}

BOOL container_object::query_user_table(const PROPTAG_ARRAY *pproptags,
	uint32_t start_pos, int32_t row_needed, TARRAY_SET *pset)
{
	auto pcontainer = this;
	BOOL b_forward;
	uint32_t first_pos;
	uint32_t row_count;
	SINGLE_LIST_NODE *psnode;
	SIMPLE_TREE_NODE *ptnode;
	TPROPVAL_ARRAY *ppropvals;
	
	if (0 == row_needed) {
		pset->count = 0;
		pset->pparray = NULL;
		return TRUE;
	} else if (row_needed > 0) {
		b_forward = TRUE;
		first_pos = start_pos;
		row_count = row_needed;
	} else {
		b_forward = FALSE;
		if (static_cast<int64_t>(start_pos) + 1 + row_needed < 0) {
			first_pos = 0;
			row_count = start_pos + 1;
		} else {
			first_pos = start_pos + 1 + row_needed;
			row_count = -row_needed;
		}
	}
	pset->count = 0;
	pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(row_count);
	if (NULL == pset->pparray) {
		return FALSE;
	}
	if (CONTAINER_TYPE_ABTREE == pcontainer->type) {
		if (NULL != pcontainer->contents.pminid_array &&
			0 == pcontainer->contents.pminid_array->count) {
			pset->count = 0;
			pset->pparray = NULL;
			return TRUE;
		}
		auto pbase = ab_tree_get_base(pcontainer->id.abtree_id.base_id);
		if (pbase == nullptr)
			return FALSE;
		if (NULL != pcontainer->contents.pminid_array) {
			for (size_t i = first_pos; i < first_pos+row_count &&
			     i < pcontainer->contents.pminid_array->count; ++i) {
				ptnode = ab_tree_minid_to_node(pbase.get(),
					pcontainer->contents.pminid_array->pl[i]);
				if (NULL == ptnode) {
					continue;
				}
				pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
				if (NULL == pset->pparray[pset->count]) {
					return FALSE;
				}
				if (FALSE == ab_tree_fetch_node_properties(
					ptnode, pproptags, pset->pparray[pset->count])) {
					return FALSE;	
				}
				pset->count ++;
			}
		} else {
			if (0xFFFFFFFF == pcontainer->id.abtree_id.minid) {
				size_t i = 0;
				for (psnode = single_list_get_head(&pbase->gal_list);
				     psnode != nullptr;
				     psnode = single_list_get_after(&pbase->gal_list, psnode), ++i) {
					if (i < first_pos) {
						continue;
					}
					pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
					if (NULL == pset->pparray[pset->count]) {
						return FALSE;
					}
					if (!ab_tree_fetch_node_properties(static_cast<SIMPLE_TREE_NODE *>(psnode->pdata),
					    pproptags, pset->pparray[pset->count])) {
						return FALSE;	
					}
					pset->count ++;
					if (pset->count == row_count) {
						break;
					}
				}
			} else if (0 == pcontainer->id.abtree_id.minid) {
				return TRUE;
			} else {
				ptnode = ab_tree_minid_to_node(pbase.get(),
					pcontainer->id.abtree_id.minid);
				if (NULL == ptnode || NULL == (ptnode =
					simple_tree_node_get_child(ptnode))) {
					return TRUE;
				}
				size_t i = 0;
				do {
					if (ab_tree_get_node_type(ptnode) > 0x80) {
						continue;
					}
					if (i < first_pos) {
						continue;
					}
					i ++;
					pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
					if (NULL == pset->pparray[pset->count]) {
						return FALSE;
					}
					if (FALSE == ab_tree_fetch_node_properties(
						ptnode, pproptags, pset->pparray[pset->count])) {
						return FALSE;	
					}
					pset->count ++;
					if (pset->count == row_count) {
						break;
					}
				} while ((ptnode = simple_tree_node_get_sibling(ptnode)) != nullptr);
			}
		}
	} else {
		if (NULL == pcontainer->contents.prow_set) {
			if (!pcontainer->load_user_table(nullptr))
				return FALSE;	
		}
		if (pcontainer->contents.prow_set != nullptr) {
			for (size_t i = first_pos;
			     i < pcontainer->contents.prow_set->count &&
			     i < first_pos+row_count; ++i) {
				pset->pparray[pset->count] =
					pcontainer->contents.prow_set->pparray[i];
				pset->count++;
			}
		}
	}
	if (FALSE == b_forward) {
		for (size_t i = 0; i < pset->count / 2; ++i) {
			ppropvals = pset->pparray[i];
			pset->pparray[i] = pset->pparray[pset->count - 1 - i];
			pset->pparray[pset->count - 1 - i] = ppropvals;
		}
	}
	return TRUE;
}
