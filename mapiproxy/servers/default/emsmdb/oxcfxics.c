/*
   OpenChange Server implementation

   EMSMDBP: EMSMDB Provider implementation

   Copyright (C) Wolfgang Sourdeau 2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
   \file oxcfxics.c

   \brief FastTransfer and ICS object routines and Rops
 */

#include "mapiproxy/libmapiserver/libmapiserver.h"
#include "dcesrv_exchange_emsmdb.h"
#include "gen_ndr/ndr_exchange.h"

/* a constant time offset by which the first change number ever can be produced by OpenChange */
#define oc_version_time 0x4dbb2dbe

/** notes:
 * conventions:
 - binary data must be returned as Binary_r
 - PR_CHANGE_NUM is computed
 - PR_CHANGE_KEY, PR_SOURCE_KEY, PR_PARENT_SOURCE_KEY are deduced automatically from PR_CHANGE_NUM, PR_MID/PR_FID and PR_PARENT_FID
 * PR_*KEY should be computed in the same manner in oxcprpt and oxctabl
 - PR_PREDECESSOR_CHANGE_LIST is only a copy or PR_CHANGE_KEY for now
 - all string properties are fetched via their _UNICODE version
 - "PR_LAST_MODIFICATION_TIME" is left to the backend, maybe setprops operations could provide an optional one, for reference...
 ? idea: getprops on tables and objects without property array = get all props
 * no deletions yet
 * no CUT markers!
 * no conflict resolution
 * ImportHierarchyChange require the same changes as RopOpenFolder with regards to opening folder and mapistore v2 functionality

 * there is a hack with get props and get table props for root mapistore folders, that can be solved with mapistore v2
 * another missing feature (3.3.5.5.4.1.1): "A move of a folder from one
 parent to another is modeled as a modification of a folder, where the value
 of PidTagParentSourceKey of the folder changes to reflect the new parent."

 * HACK: CnsetSeen = CnsetSeenFAI = CnsetRead */

struct oxcfxics_prop_index {
	uint32_t	parent_fid;
	uint32_t	eid;
	uint32_t	precedessor_change_list;
	uint32_t	last_modification_time;
	uint32_t	display_name;
	uint32_t	associated;
	uint32_t	message_size;
};

struct oxcfxics_sync_data {
	struct GUID			replica_guid;
	uint8_t				table_type;
	struct oxcfxics_prop_index	prop_index;
	struct SPropTagArray		properties;

	struct ndr_push			*ndr;
	struct ndr_push			*cutmarks_ndr;

	struct rawidset			*eid_set;
	struct rawidset			*cnset_seen;
};

/** ndr helpers */

static void oxcfxics_ndr_push_simple_data(struct ndr_push *ndr, uint16_t data_type, const void *value)
{
	uint32_t	string_len;

	switch (data_type) {
	case PT_I2:
		ndr_push_uint16(ndr, NDR_SCALARS, *(uint16_t *) value);
		break;
	case PT_LONG:
	case PT_ERROR:
	case PT_OBJECT:
		ndr_push_uint32(ndr, NDR_SCALARS, *(uint32_t *) value);
		break;
	case PT_DOUBLE:
		ndr_push_double(ndr, NDR_SCALARS, *(double *) value);
		break;
	case PT_I8:
		ndr_push_dlong(ndr, NDR_SCALARS, *(uint64_t *) value);
		break;
	case PT_BOOLEAN:
		if (*(uint8_t *) value) {
			ndr_push_uint16(ndr, NDR_SCALARS, 1);
		}
		else {
			ndr_push_uint16(ndr, NDR_SCALARS, 0);
		}
		break;
	case PT_STRING8:
		string_len = strlen(value) + 1;
		ndr_push_uint32(ndr, NDR_SCALARS, string_len);
		ndr_set_flags(&ndr->flags, LIBNDR_FLAG_STR_NULLTERM|LIBNDR_FLAG_STR_ASCII);
		ndr_push_string(ndr, NDR_SCALARS, (char *) value);
		break;
	case PT_UNICODE:
		string_len = strlen_m_ext((char *) value, CH_UTF8, CH_UTF16LE) * 2 + 2;
		ndr_push_uint32(ndr, NDR_SCALARS, string_len);
		ndr_set_flags(&ndr->flags, LIBNDR_FLAG_STR_NULLTERM);
		ndr_push_string(ndr, NDR_SCALARS, (char *) value);
		break;
	case PT_SVREID:
	case PT_BINARY:
		ndr_push_Binary_r(ndr, NDR_BUFFERS, (struct Binary_r *) value);
		break;
	case PT_CLSID:
		ndr_push_GUID(ndr, NDR_SCALARS, (struct GUID *) value);
		break;
	case PT_SYSTIME:
		ndr_push_FILETIME(ndr, NDR_SCALARS, (struct FILETIME *) value);
		break;
	default:
		DEBUG(5, ("%s: unsupported property type: %.4x\n", __FUNCTION__, data_type));
		abort();
	}
}

static void oxcfxics_ndr_push_properties(struct ndr_push *ndr, struct ndr_push *cutmarks_ndr, void *nprops_ctx, struct SPropTagArray *properties, void **data_pointers, enum MAPISTATUS *retvals)
{
	uint32_t		i, j;
	enum MAPITAGS		property;
        struct MAPINAMEID       *nameid;
	struct BinaryArray_r	*bin_array;
	struct WStringArray_r	*unicode_array;
	uint16_t		prop_type, propID;
        int                     retval;
	void			*mem_ctx;

	mem_ctx = talloc_zero(NULL, void);

        for (i = 0; i < properties->cValues; i++) {
                if (retvals[i] == MAPI_E_SUCCESS) {
			ndr_push_uint32(cutmarks_ndr, NDR_SCALARS, ndr->offset);

                        property = properties->aulPropTag[i];
			if (property > 0x80000000) {
				propID = (property & 0xffff0000) >> 16;
				retval = mapistore_namedprops_get_nameid(mem_ctx, nprops_ctx, propID, &nameid);
				if (retval != MAPISTORE_SUCCESS) {
					continue;
				}
				ndr_push_uint32(ndr, NDR_SCALARS, property);
				ndr_push_GUID(ndr, NDR_SCALARS, &nameid->lpguid);
				switch (nameid->ulKind) {
				case MNID_ID:
					ndr_push_uint8(ndr, NDR_SCALARS, 0);
					ndr_push_uint32(ndr, NDR_SCALARS, nameid->kind.lid);
					break;
				case MNID_STRING:
					ndr_push_uint8(ndr, NDR_SCALARS, 1);
					ndr_set_flags(&ndr->flags, LIBNDR_FLAG_STR_NULLTERM);
					ndr_push_string(ndr, NDR_SCALARS, nameid->kind.lpwstr.Name);
					break;
				}
				talloc_free(nameid);
			} else {
				ndr_push_uint32(ndr, NDR_SCALARS, property);
			}

			prop_type = property & 0xffff;
			if ((prop_type & MV_FLAG)) {
				prop_type &= 0x0fff;

				switch (prop_type) {
				case PT_BINARY:
					bin_array = data_pointers[i];
					ndr_push_uint32(ndr, NDR_SCALARS, bin_array->cValues);
					for (j = 0; j < bin_array->cValues; j++) {
						oxcfxics_ndr_push_simple_data(ndr, prop_type, bin_array->lpbin + j);
					}
					break;
				case PT_UNICODE:
					unicode_array = data_pointers[i];
					ndr_push_uint32(ndr, NDR_SCALARS, unicode_array->cValues);
					for (j = 0; j < unicode_array->cValues; j++) {
						oxcfxics_ndr_push_simple_data(ndr, prop_type, unicode_array->lppszW[j]);
					}
					break;
				default:
					DEBUG(5, ("No handling for multi values of type %.4x\n", prop_type));
					abort();
				}
			}
			else {
				oxcfxics_ndr_push_simple_data(ndr, prop_type, data_pointers[i]);
			}
		}
        }

	talloc_free(mem_ctx);
}

static int oxcfxics_fmid_from_source_key(struct emsmdbp_context *emsmdbp_ctx, struct SBinary_short *source_key, uint64_t *fmidp)
{
	uint64_t	fmid, base;
	uint16_t	replid;
	const uint8_t	*bytes;
	int		i;

	if (emsmdbp_guid_to_replid(emsmdbp_ctx, (const struct GUID *) source_key->lpb, &replid)) {
		return MAPISTORE_ERROR;
	}

	bytes = source_key->lpb + 16;
	fmid = 0;
	base = 1;
	for (i = 0; i < 6; i++) {
		fmid |= (uint64_t) bytes[i] * base;
		base <<= 8;
	}
	fmid <<= 16;
	fmid |= replid;
	*fmidp = fmid;

	return MAPISTORE_SUCCESS;
}

static int oxcfxics_source_key_from_fmid(TALLOC_CTX *mem_ctx, struct emsmdbp_context *emsmdbp_ctx, uint64_t fmid, struct Binary_r **source_keyP)
{
	struct Binary_r	*source_key;
	uint64_t	gc;
	uint16_t	replid;
	uint8_t		*bytes;
	int		i;

	replid = fmid & 0xffff;
	source_key = talloc_zero(NULL, struct Binary_r);
	source_key->cb = 22;
	source_key->lpb = talloc_array(source_key, uint8_t, source_key->cb);
	if (emsmdbp_replid_to_guid(emsmdbp_ctx, replid, (struct GUID *) source_key->lpb)) {
		talloc_free(source_key);
		return MAPISTORE_ERROR;
	}

	(void) talloc_reference(mem_ctx, source_key);
	talloc_free(source_key);

	gc = fmid >> 16;

	bytes = source_key->lpb + 16;
	for (i = 0; i < 6; i++) {
		bytes[i] = gc & 0xff;
		gc >>= 8;
	}

	*source_keyP = source_key;

	return MAPISTORE_SUCCESS;
}

static struct Binary_r *oxcfxics_make_xid(TALLOC_CTX *mem_ctx, struct GUID *replica_guid, uint64_t *id, uint8_t idlength)
{
	struct ndr_push	*ndr;
	struct Binary_r *data;
	uint8_t	i;
	uint64_t current_id;

	if (!mem_ctx) return NULL;
	if (!replica_guid || !id) return NULL;
	if (idlength > 8) return NULL;

	/* GUID */
	ndr = ndr_push_init_ctx(NULL);
	ndr_set_flags(&ndr->flags, LIBNDR_FLAG_NOALIGN);
	ndr->offset = 0;
	ndr_push_GUID(ndr, NDR_SCALARS, replica_guid);

	/* id */
	current_id = *id;
	for (i = 0; i < idlength; i++) {
		ndr_push_uint8(ndr, NDR_SCALARS, current_id & 0xff);
		current_id >>= 8;
	}

	data = talloc_zero(mem_ctx, struct Binary_r);
	data->cb = ndr->offset;
	data->lpb = ndr->data;
	(void) talloc_reference(data, ndr->data);
	talloc_free(ndr);

	return data;
}

static inline struct Binary_r *oxcfxics_make_gid(TALLOC_CTX *mem_ctx, struct GUID *replica_guid, uint64_t id)
{
	return oxcfxics_make_xid(mem_ctx, replica_guid, &id, 6);
}

/**
   \details EcDoRpc EcDoRpc_RopFastTransferSourceCopyTo (0x4d) Rop. This operation initializes a FastTransfer operation to download content from a given messaging object and its descendant subobjects.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the EcDoRpc_RopFastTransferSourceCopyTo EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the EcDoRpc_RopFastTransferSourceCopyTo EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopFastTransferSourceCopyTo(TALLOC_CTX *mem_ctx,
							     struct emsmdbp_context *emsmdbp_ctx,
							     struct EcDoRpc_MAPI_REQ *mapi_req,
							     struct EcDoRpc_MAPI_REPL *mapi_repl,
							     uint32_t *handles, uint16_t *size)
{
	enum MAPISTATUS				retval;
	struct mapi_handles			*parent_object_handle = NULL, *object_handle;
	struct emsmdbp_object			*parent_object = NULL, *object;
	struct FastTransferSourceCopyTo_req	 *request;
	uint32_t				parent_handle_id, i;
	void					*data;
	struct SPropTagArray			*needed_properties;
	void					**data_pointers;
	enum MAPISTATUS				*retvals;
	struct ndr_push				*ndr, *cutmarks_ndr;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] FastTransferSourceCopyTo (0x4d)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	request = &mapi_req->u.mapi_FastTransferSourceCopyTo;

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = request->handle_idx;

	/* Step 1. Retrieve object handle */
	parent_handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, parent_handle_id, &parent_object_handle);
	if (retval) {
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		DEBUG(5, ("  handle (%x) not found: %x\n", parent_handle_id, mapi_req->handle_idx));
		goto end;
	}

	/* Step 2. Check whether the parent object supports fetching properties */
	mapi_handles_get_private_data(parent_object_handle, &data);
	parent_object = (struct emsmdbp_object *) data;

	if (request->Level > 0) {
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		DEBUG(5, ("  no support for levels > 0\n"));
                goto end;
	}

	if (emsmdbp_object_get_available_properties(mem_ctx, emsmdbp_ctx, parent_object, &needed_properties) == MAPISTORE_SUCCESS) {
		if (needed_properties->cValues > 0) {
			for (i = 0; i < request->PropertyTags.cValues; i++) {
				SPropTagArray_delete(mem_ctx, needed_properties, request->PropertyTags.aulPropTag[i]);
			}

			data_pointers = emsmdbp_object_get_properties(mem_ctx, emsmdbp_ctx, parent_object, needed_properties, &retvals);
			if (data_pointers == NULL) {
				mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
				DEBUG(5, ("  unexpected error\n"));
				goto end;
			}

			ndr = ndr_push_init_ctx(NULL);
			ndr_set_flags(&ndr->flags, LIBNDR_FLAG_NOALIGN);
			ndr->offset = 0;

			cutmarks_ndr = ndr_push_init_ctx(NULL);
			ndr_set_flags(&cutmarks_ndr->flags, LIBNDR_FLAG_NOALIGN);
			cutmarks_ndr->offset = 0;

			oxcfxics_ndr_push_properties(ndr, cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, needed_properties, data_pointers, retvals);

			retval = mapi_handles_add(emsmdbp_ctx->handles_ctx, parent_handle_id, &object_handle);
			object = emsmdbp_object_ftcontext_init(object_handle, emsmdbp_ctx, parent_object);
			if (object == NULL) {
				mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
				DEBUG(5, ("  context object not created\n"));
				goto end;
			}

			ndr_push_uint32(cutmarks_ndr, NDR_SCALARS, 0xffffffff);

			(void) talloc_reference(object, ndr->data);
			(void) talloc_reference(object, cutmarks_ndr->data);

			object->object.ftcontext->cutmarks = (uint32_t *) cutmarks_ndr->data;
			object->object.ftcontext->stream.buffer.data = ndr->data;
			object->object.ftcontext->stream.buffer.length = ndr->offset;

			talloc_free(ndr);
			talloc_free(cutmarks_ndr);

			mapi_handles_set_private_data(object_handle, object);
			handles[mapi_repl->handle_idx] = object_handle->handle;

			talloc_free(data_pointers);
			talloc_free(retvals);
		}
	}

end:
	*size += libmapiserver_RopFastTransferSourceCopyTo_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

static void oxcfxics_push_messageChange_recipients(TALLOC_CTX *mem_ctx, struct emsmdbp_context *emsmdbp_ctx, struct oxcfxics_sync_data *sync_data, struct emsmdbp_object *message_object, struct mapistore_message *msg)
{
	uint32_t		i, j;
	struct SPropTagArray	properties;
	void			**data_pointers;
	enum MAPISTATUS		*retvals;
	void			*row_ctx;

	ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_FX_DEL_PROP);
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_MESSAGE_RECIPIENTS);

	if (msg && msg->recipients) {
		for (i = 0; i < msg->recipients->cRows; i++) {
			ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);
			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_START_RECIP);
			ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);
			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_ROWID);
			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, i);

			row_ctx = talloc_zero(NULL, void);

			properties.cValues = msg->recipients->aRow[i].cValues;
			properties.aulPropTag = talloc_array(mem_ctx, enum MAPITAGS, properties.cValues);

			data_pointers = talloc_array(row_ctx, void *, properties.cValues);
			retvals = talloc_array(row_ctx, enum MAPISTATUS, properties.cValues);
			memset(retvals, 0, properties.cValues * sizeof(enum MAPISTATUS));
			for (j = 0; j < msg->recipients->aRow[i].cValues; j++) {
				properties.aulPropTag[j] = msg->recipients->aRow[i].lpProps[j].ulPropTag;
				data_pointers[j] = (void *) get_SPropValue_data(msg->recipients->aRow[i].lpProps + j);
			}
			oxcfxics_ndr_push_properties(sync_data->ndr, sync_data->cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, &properties, data_pointers, retvals);
			ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);
			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_END_RECIP);

			talloc_free(row_ctx);
		}
	}
}

static void oxcfxics_push_messageChange_attachments(TALLOC_CTX *mem_ctx, struct emsmdbp_context *emsmdbp_ctx, struct oxcfxics_sync_data *sync_data, struct emsmdbp_object *message_object)
{
	struct emsmdbp_object	*table_object;
	TALLOC_CTX		*local_mem_ctx;
	static const int	prop_count = 11;
	static enum MAPITAGS	prop_tags[] = { PR_ATTACH_METHOD, PR_ATTACH_TAG, PR_ATTACH_SIZE, PR_RECORD_KEY, PR_ATTACH_LONG_FILENAME_UNICODE, PR_DISPLAY_NAME_UNICODE, PR_ATTACH_CONTENT_ID_UNICODE, PR_ATTACH_MIME_TAG_UNICODE, PR_CREATION_TIME, PR_LAST_MODIFICATION_TIME, PR_ATTACH_DATA_BIN };
	struct SPropTagArray	query_props;
	uint32_t		i;
	uint32_t		*retvals;
	void			**data_pointers;

	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_FX_DEL_PROP);
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_MESSAGE_ATTACHMENTS);

	table_object = emsmdbp_object_message_open_attachment_table(NULL, emsmdbp_ctx, message_object);
	if (table_object && table_object->object.table->denominator > 0) {
		table_object->object.table->properties = prop_tags;
		table_object->object.table->prop_count = prop_count;
		if (emsmdbp_is_mapistore(table_object)) {
			mapistore_table_set_columns(emsmdbp_ctx->mstore_ctx, emsmdbp_get_contextID(table_object), table_object->backend_object, prop_count, prop_tags);
		}
		for (i = 0; i < table_object->object.table->denominator; i++) {
			local_mem_ctx = talloc_zero(NULL, void);
			data_pointers = emsmdbp_object_table_get_row_props(local_mem_ctx, emsmdbp_ctx, table_object, i, &retvals);
			if (data_pointers) {
				ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);
				ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_NEW_ATTACH);
				ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_ATTACH_NUM);
				ndr_push_uint32(sync_data->ndr, NDR_SCALARS, i);
				query_props.cValues = prop_count;
				query_props.aulPropTag = prop_tags;
				oxcfxics_ndr_push_properties(sync_data->ndr, sync_data->cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, &query_props, data_pointers, (enum MAPISTATUS *) retvals);
				ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_END_ATTACH);
			}
			else {
				DEBUG(5, ("no data returned for attachment row %d\n", i));
				abort();
			}
			talloc_free(local_mem_ctx);
		}
	}

	talloc_free(table_object);
}

static void oxcfxics_push_messageChange(TALLOC_CTX *mem_ctx, struct emsmdbp_context *emsmdbp_ctx, struct emsmdbp_object_synccontext *synccontext, struct oxcfxics_sync_data *sync_data, struct emsmdbp_object *folder_object)
{
	struct emsmdbp_object		*table_object, *message_object;
	uint32_t			i, j;
	uint32_t			*retvals, *header_retvals;
	void				**data_pointers, **header_data_pointers;
	struct FILETIME			*lm_time;
	NTTIME				nt_time;
	uint32_t			unix_time;
	struct SPropTagArray		query_props;
	struct Binary_r			predecessors_data;
	struct Binary_r			*bin_data;
	uint64_t			eid, cn;
	TALLOC_CTX			*local_mem_ctx;
	struct mapistore_message	*msg;
	struct GUID			replica_guid;

	/* we only push "messageChangeFull" since we don't handle property-based changes */
	/* messageChangeFull = IncrSyncChg messageChangeHeader IncrSyncMessage propList messageChildren */

	local_mem_ctx = talloc_zero(NULL, void);

 	table_object = emsmdbp_folder_open_table(local_mem_ctx, folder_object, sync_data->table_type, 0); 
	if (!table_object) {
		DEBUG(5, ("could not open folder table\n"));
		abort();
	}
	table_object->object.table->prop_count = sync_data->properties.cValues;
	table_object->object.table->properties = sync_data->properties.aulPropTag;
	if (emsmdbp_is_mapistore(table_object)) {
		mapistore_table_set_columns(emsmdbp_ctx->mstore_ctx, emsmdbp_get_contextID(table_object), table_object->backend_object, sync_data->properties.cValues, sync_data->properties.aulPropTag);
	}
	for (i = 0; i < table_object->object.table->denominator; i++) {
		data_pointers = emsmdbp_object_table_get_row_props(mem_ctx, emsmdbp_ctx, table_object, i, &retvals);
		if (data_pointers) {
			ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);

			/** fixed header props */
			header_data_pointers = talloc_array(NULL, void *, 8);
			header_retvals = talloc_array(header_data_pointers, uint32_t, 8);
			memset(header_retvals, 0, 8 * sizeof(uint32_t));
			query_props.aulPropTag = talloc_array(header_data_pointers, enum MAPITAGS, 8);
				
			j = 0;

			/* source key */
			eid = *(uint64_t *) data_pointers[sync_data->prop_index.eid];
			emsmdbp_replid_to_guid(emsmdbp_ctx, eid & 0xffff, &replica_guid);
			RAWIDSET_push_glob(sync_data->eid_set, &replica_guid, eid >> 16);

			/* bin_data = oxcfxics_make_gid(header_data_pointers, &sync_data->replica_guid, eid >> 16); */
			oxcfxics_source_key_from_fmid(header_data_pointers, emsmdbp_ctx, eid, &bin_data);
			query_props.aulPropTag[j] = PR_SOURCE_KEY;
			header_data_pointers[j] = bin_data;
			j++;

			/* last modification time */
			if (retvals[sync_data->prop_index.last_modification_time]) {
				unix_time = oc_version_time;
				unix_to_nt_time(&nt_time, unix_time);
				lm_time = talloc_zero(data_pointers, struct FILETIME);
				lm_time->dwLowDateTime = (nt_time & 0xffffffff);
				lm_time->dwHighDateTime = nt_time >> 32;
			}
			else {
				lm_time = (struct FILETIME *) data_pointers[sync_data->prop_index.last_modification_time];
				nt_time = ((uint64_t) lm_time->dwHighDateTime << 32) | lm_time->dwLowDateTime;
				unix_time = nt_time_to_unix(nt_time);
			}
			query_props.aulPropTag[j] = PR_LAST_MODIFICATION_TIME;
			header_data_pointers[j] = lm_time;
			j++;

			if (unix_time < oc_version_time) {
				unix_time = oc_version_time;
			}
			cn = ((eid & 0xffff000000000000LL) >> 16) | (exchange_globcnt(unix_time - oc_version_time) >> 16);
			if (IDSET_includes_id(synccontext->cnset_seen, &sync_data->replica_guid, cn)) {
				DEBUG(5, ("WARNING: cn %.16"PRIx64" already present\n", cn));
				goto end_row;
			}
			/* The "cnset_seen" range is going to be merged later with the one from emsmdb_ctx since the ids are not sorted */
			RAWIDSET_push_glob(sync_data->cnset_seen, &sync_data->replica_guid, cn);

			/* change key */
			bin_data = oxcfxics_make_gid(header_data_pointers, &sync_data->replica_guid, cn);
			query_props.aulPropTag[j] = PR_CHANGE_KEY;
			header_data_pointers[j] = bin_data;
			j++;
				
			/* predecessor... (already computed) */
			predecessors_data.cb = bin_data->cb + 1;
			predecessors_data.lpb = talloc_array(header_data_pointers, uint8_t, predecessors_data.cb);
			*predecessors_data.lpb = bin_data->cb & 0xff;
			memcpy(predecessors_data.lpb + 1, bin_data->lpb, bin_data->cb);
			query_props.aulPropTag[j] = PR_PREDECESSOR_CHANGE_LIST;
			header_data_pointers[j] = &predecessors_data;
			j++;

			/* associated (could be based on table type ) */
			query_props.aulPropTag[j] = PR_ASSOCIATED;
			if (retvals[sync_data->prop_index.associated]) {
				header_data_pointers[j] = talloc_zero(header_data_pointers, uint8_t);
			}
			else {
				header_data_pointers[j] = data_pointers[sync_data->prop_index.associated];
			}
			j++;

			/* message id (conditional) */
			if (synccontext->request.request_eid) {
				query_props.aulPropTag[j] = PR_MID;
				header_data_pointers[j] = &eid;
				j++;
			}

			/* message size (conditional) */
			if (synccontext->request.request_message_size) {
				query_props.aulPropTag[j] = PR_MESSAGE_SIZE;
				header_data_pointers[j] = data_pointers[sync_data->prop_index.message_size];
				if (retvals[sync_data->prop_index.parent_fid]) {
					header_data_pointers[j] = talloc_zero(header_data_pointers, uint32_t);
				}
				else {
					header_data_pointers[j] = data_pointers[sync_data->prop_index.message_size];
				}
				j++;
			}

			/* cn (conditional) */
			if (synccontext->request.request_cn) {
				query_props.aulPropTag[j] = PR_CHANGE_NUM;
				header_data_pointers[j] = talloc_zero(header_data_pointers, uint64_t);
				*(uint64_t *) header_data_pointers[j] = (cn << 16) | (eid & 0xffff);
				j++;
			}

			query_props.cValues = j;

			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_CHG);
			oxcfxics_ndr_push_properties(sync_data->ndr, sync_data->cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, &query_props, header_data_pointers, (enum MAPISTATUS *) header_retvals);
			/** remaining props */
			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_MSG);

			if (table_object->object.table->prop_count > 7) {
				query_props.cValues = table_object->object.table->prop_count - 7;
				query_props.aulPropTag = table_object->object.table->properties + 7;
				oxcfxics_ndr_push_properties(sync_data->ndr, sync_data->cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, &query_props, data_pointers + 7, (enum MAPISTATUS *) retvals + 7);
			}

			/* messageChildren:
			   [ PidTagFXDelProp ] [ *(StartRecip propList EndToRecip) ] [ PidTagFXDelProp ] [ *(NewAttach propList [embeddedMessage] EndAttach) ]
			   embeddedMessage:
			   StartEmbed messageContent EndEmbed */

			message_object = emsmdbp_object_message_open(NULL, emsmdbp_ctx, folder_object, folder_object->object.folder->folderID, eid, &msg);
			oxcfxics_push_messageChange_recipients(mem_ctx, emsmdbp_ctx, sync_data, message_object, msg);
			oxcfxics_push_messageChange_attachments(mem_ctx, emsmdbp_ctx, sync_data, message_object);
			talloc_free(message_object);

		end_row:
			talloc_free(header_data_pointers);
			talloc_free(retvals);
			talloc_free(data_pointers);
		}
		else {
			DEBUG(5, ("no data returned for message row %d\n", i));
			abort();
		}
	}

	talloc_free(local_mem_ctx);
}

static void oxcfxics_prepare_synccontext_with_messageChange(TALLOC_CTX *mem_ctx, struct emsmdbp_object *synccontext_object)
{
	struct oxcfxics_sync_data		*sync_data;
	struct idset				*new_idset, *old_idset;
	struct emsmdbp_object_synccontext	*synccontext;
	struct emsmdbp_context			*emsmdbp_ctx;

	/* contentsSync = [progressTotal] *( [progressPerMessage] messageChange ) [deletions] [readStateChanges] state IncrSyncEnd */

	/* 1. we setup the mandatory properties indexes */
	emsmdbp_ctx = synccontext_object->emsmdbp_ctx;
	synccontext = synccontext_object->object.synccontext;
	sync_data = talloc_zero(NULL, struct oxcfxics_sync_data);
	openchangedb_get_MailboxReplica(emsmdbp_ctx->oc_ctx, emsmdbp_ctx->username, NULL, &sync_data->replica_guid);
	sync_data->properties = synccontext->properties;
	SPropTagArray_find(synccontext->properties, PR_MID, &sync_data->prop_index.eid);
	SPropTagArray_find(synccontext->properties, PR_LAST_MODIFICATION_TIME, &sync_data->prop_index.last_modification_time);
	SPropTagArray_find(synccontext->properties, PR_PREDECESSOR_CHANGE_LIST, &sync_data->prop_index.precedessor_change_list);
	SPropTagArray_find(synccontext->properties, PR_ASSOCIATED, &sync_data->prop_index.associated);
	SPropTagArray_find(synccontext->properties, PR_MESSAGE_SIZE, &sync_data->prop_index.message_size);
	sync_data->ndr = ndr_push_init_ctx(sync_data);
	ndr_set_flags(&sync_data->ndr->flags, LIBNDR_FLAG_NOALIGN);
	sync_data->ndr->offset = 0;
	sync_data->cutmarks_ndr = ndr_push_init_ctx(sync_data);
	ndr_set_flags(&sync_data->cutmarks_ndr->flags, LIBNDR_FLAG_NOALIGN);
	sync_data->cutmarks_ndr->offset = 0;
	sync_data->cnset_seen = RAWIDSET_make(sync_data, true);
	sync_data->eid_set = RAWIDSET_make(sync_data, false);

	/* 2a. we build the message stream (normal messages) */
	if (synccontext->request.normal) {
		sync_data->table_type = EMSMDBP_TABLE_MESSAGE_TYPE;
		oxcfxics_push_messageChange(mem_ctx, emsmdbp_ctx, synccontext, sync_data, synccontext_object->parent_object);
	}

	/* 2b. we build the message stream (FAI messages) */
	if (synccontext->request.fai) {
		sync_data->table_type = EMSMDBP_TABLE_FAI_TYPE;
		oxcfxics_push_messageChange(mem_ctx, emsmdbp_ctx, synccontext, sync_data, synccontext_object->parent_object);
	}

	/* state */
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_STATE_BEGIN);

	new_idset = RAWIDSET_convert_to_idset(NULL, sync_data->eid_set);
	old_idset = synccontext->idset_given;
	synccontext->idset_given = IDSET_merge_idsets(synccontext, old_idset, new_idset);
	talloc_free(old_idset);
	talloc_free(new_idset);

	new_idset = RAWIDSET_convert_to_idset(NULL, sync_data->cnset_seen);
	old_idset = synccontext->cnset_seen;
	synccontext->cnset_seen = IDSET_merge_idsets(synccontext, old_idset, new_idset);
	talloc_free(old_idset);
	talloc_free(new_idset);

	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PidTagCnsetSeen);
	ndr_push_idset(sync_data->ndr, synccontext->cnset_seen);

	if (synccontext->request.fai) {
		ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PidTagCnsetSeenFAI);
		ndr_push_idset(sync_data->ndr, synccontext->cnset_seen);
	}
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PidTagIdsetGiven);
	ndr_push_idset(sync_data->ndr, synccontext->idset_given);
	if (synccontext->request.read_state) {
		ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PidTagCnsetRead);
		ndr_push_idset(sync_data->ndr, synccontext->cnset_seen);
	}
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_STATE_END);

	/* end of stream */
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_END);
	ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, 0xffffffff);

	(void) talloc_reference(synccontext, sync_data->ndr->data);
	(void) talloc_reference(synccontext, sync_data->cutmarks_ndr->data);
	synccontext->cutmarks = (uint32_t *) sync_data->cutmarks_ndr->data;
	synccontext->stream.buffer.data = sync_data->ndr->data;
	synccontext->stream.buffer.length = sync_data->ndr->offset;

	talloc_free(sync_data);
}

static void oxcfxics_push_folderChange(TALLOC_CTX *mem_ctx, struct emsmdbp_context *emsmdbp_ctx, struct emsmdbp_object_synccontext *synccontext, struct emsmdbp_object *topmost_folder_object, struct oxcfxics_sync_data *sync_data, struct emsmdbp_object *folder_object)
{
	struct emsmdbp_object	*table_object, *subfolder_object;
	uint64_t		eid, cn;
	struct Binary_r		predecessors_data;
	struct Binary_r		*bin_data;
	struct FILETIME		*lm_time;
	NTTIME			nt_time;
	int32_t			unix_time;
	uint32_t		i, j;
	uint32_t		*retvals, *header_retvals;
	void			**data_pointers, **header_data_pointers;
	struct SPropTagArray	query_props;
	TALLOC_CTX		*local_mem_ctx;
	struct GUID		replica_guid;

	local_mem_ctx = talloc_zero(NULL, void);

	/* 2b. we build the stream */
	table_object = emsmdbp_folder_open_table(local_mem_ctx, folder_object, EMSMDBP_TABLE_FOLDER_TYPE, 0); 
	if (!table_object) {
		DEBUG(5, ("folder does not handle hierarchy tables\n"));
		return;
	}

	table_object->object.table->prop_count = sync_data->properties.cValues;
	table_object->object.table->properties = sync_data->properties.aulPropTag;
	if (emsmdbp_is_mapistore(table_object)) {
		mapistore_table_set_columns(emsmdbp_ctx->mstore_ctx, emsmdbp_get_contextID(table_object),
					    table_object->backend_object, sync_data->properties.cValues, sync_data->properties.aulPropTag);
	}

	for (i = 0; i < table_object->object.table->denominator; i++) {
		data_pointers = emsmdbp_object_table_get_row_props(mem_ctx, emsmdbp_ctx, table_object, i, &retvals);
		if (data_pointers) {
			ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, sync_data->ndr->offset);

			/** fixed header props */
			header_data_pointers = talloc_array(NULL, void *, 8);
			header_retvals = talloc_array(NULL, uint32_t, 8);
			memset(header_retvals, 0, 8 * sizeof(uint32_t));
			query_props.aulPropTag = talloc_array(header_data_pointers, enum MAPITAGS, 8);
				
			j = 0;

			/* parent source key */
			if (folder_object == topmost_folder_object) {
				/* No parent source key at the first hierarchy level */
				bin_data = talloc_zero(header_data_pointers, struct Binary_r);
				bin_data->lpb = (uint8_t *) "";
			}
			else {
				oxcfxics_source_key_from_fmid(header_data_pointers, emsmdbp_ctx, *(uint64_t *) data_pointers[sync_data->prop_index.parent_fid], &bin_data);
			}
			query_props.aulPropTag[j] = PR_PARENT_SOURCE_KEY;
			header_data_pointers[j] = bin_data;
			j++;
			
			/* source key */
			eid = *(uint64_t *) data_pointers[sync_data->prop_index.eid];
			emsmdbp_replid_to_guid(emsmdbp_ctx, eid & 0xffff, &replica_guid);
			RAWIDSET_push_glob(sync_data->eid_set, &replica_guid, eid >> 16);

			/* bin_data = oxcfxics_make_gid(header_data_pointers, &sync_data->replica_guid, eid >> 16); */
			oxcfxics_source_key_from_fmid(header_data_pointers, emsmdbp_ctx, eid, &bin_data);
			query_props.aulPropTag[j] = PR_SOURCE_KEY;
			header_data_pointers[j] = bin_data;
			j++;
				
			/* last modification time */
			if (retvals[sync_data->prop_index.last_modification_time]) {
				unix_time = oc_version_time;
				unix_to_nt_time(&nt_time, unix_time);
				lm_time = talloc_zero(data_pointers, struct FILETIME);
				lm_time->dwLowDateTime = (nt_time & 0xffffffff);
				lm_time->dwHighDateTime = nt_time >> 32;
			}
			else {
				lm_time = (struct FILETIME *) data_pointers[sync_data->prop_index.last_modification_time];
				nt_time = ((uint64_t) lm_time->dwHighDateTime << 32) | lm_time->dwLowDateTime;
				unix_time = nt_time_to_unix(nt_time);
			}
			query_props.aulPropTag[j] = PR_LAST_MODIFICATION_TIME;
			header_data_pointers[j] = lm_time;
			j++;

			if (unix_time < oc_version_time) {
				unix_time = oc_version_time;
			}
			cn = ((eid & 0xffff000000000000LL) >> 16) | (exchange_globcnt(unix_time - oc_version_time) >> 16);
			if (IDSET_includes_id(synccontext->cnset_seen, &sync_data->replica_guid, cn)) {
				DEBUG(5, ("WARNING: cn %.16"PRIx64" already present\n", cn));
				goto end_row;
			}
			RAWIDSET_push_glob(sync_data->cnset_seen, &sync_data->replica_guid, cn);

			/* change key */
			bin_data = oxcfxics_make_gid(header_data_pointers, &sync_data->replica_guid, cn);
			query_props.aulPropTag[j] = PR_CHANGE_KEY;
			header_data_pointers[j] = bin_data;
			j++;
				
			/* predecessor... (already computed) */
			predecessors_data.cb = bin_data->cb + 1;
			predecessors_data.lpb = talloc_array(header_data_pointers, uint8_t, predecessors_data.cb);
			*predecessors_data.lpb = bin_data->cb & 0xff;
			memcpy(predecessors_data.lpb + 1, bin_data->lpb, bin_data->cb);
			query_props.aulPropTag[j] = PR_PREDECESSOR_CHANGE_LIST;
			header_data_pointers[j] = &predecessors_data;
			j++;
					
			/* display name */
			query_props.aulPropTag[j] = PR_DISPLAY_NAME_UNICODE;
			if (retvals[sync_data->prop_index.display_name]) {
				header_data_pointers[j] = "";
			}
			else {
				header_data_pointers[j] = data_pointers[sync_data->prop_index.display_name];
			}
			j++;
			
			/* folder id (conditional) */
			if (synccontext->request.request_eid) {
				query_props.aulPropTag[j] = PR_FID;
				header_data_pointers[j] = data_pointers[sync_data->prop_index.eid];
				j++;
			}

			/* parent folder id (conditional) */
			if (synccontext->request.no_foreign_identifiers) {
				query_props.aulPropTag[j] = PR_PARENT_FID;
				header_data_pointers[j] = data_pointers[sync_data->prop_index.parent_fid];
				if (retvals[sync_data->prop_index.parent_fid]) {
					header_data_pointers[j] = talloc_zero(header_data_pointers, uint64_t);
				}
				else {
					header_data_pointers[j] = data_pointers[sync_data->prop_index.parent_fid];
				}
				j++;
			}
			
			query_props.cValues = j;

			ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_CHG);
			oxcfxics_ndr_push_properties(sync_data->ndr, sync_data->cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, &query_props, header_data_pointers, (enum MAPISTATUS *) header_retvals);

			/** remaining props */
			if (table_object->object.table->prop_count > 5) {
				query_props.cValues = table_object->object.table->prop_count - 5;
				query_props.aulPropTag = table_object->object.table->properties + 5;
				oxcfxics_ndr_push_properties(sync_data->ndr, sync_data->cutmarks_ndr, emsmdbp_ctx->mstore_ctx->nprops_ctx, &query_props, data_pointers + 5, (enum MAPISTATUS *) retvals + 5);
			}

		end_row:
			talloc_free(header_data_pointers);
			talloc_free(header_retvals);
			talloc_free(data_pointers);
			talloc_free(retvals);

			subfolder_object = emsmdbp_object_open_folder(NULL, emsmdbp_ctx, folder_object, eid);
			oxcfxics_push_folderChange(mem_ctx, emsmdbp_ctx, synccontext, topmost_folder_object, sync_data, subfolder_object);
			talloc_free(subfolder_object);
		}
		else {
			DEBUG(5, ("no data returned for folder row %d\n", i));
			abort();
		}
	}

	talloc_free(local_mem_ctx);
}

static void oxcfxics_prepare_synccontext_with_folderChange(struct emsmdbp_object *synccontext_object)
{
	struct oxcfxics_sync_data		*sync_data;
	struct idset				*new_idset, *old_idset;
	struct emsmdbp_context			*emsmdbp_ctx;
	struct emsmdbp_object_synccontext	*synccontext;

	/* 1b. we setup context data */
	emsmdbp_ctx = synccontext_object->emsmdbp_ctx;
	synccontext = synccontext_object->object.synccontext;

	sync_data = talloc_zero(NULL, struct oxcfxics_sync_data);
	openchangedb_get_MailboxReplica(emsmdbp_ctx->oc_ctx, emsmdbp_ctx->username, NULL, &sync_data->replica_guid);
	sync_data->properties = synccontext->properties;
	SPropTagArray_find(sync_data->properties, PR_PARENT_FID, &sync_data->prop_index.parent_fid);
	SPropTagArray_find(sync_data->properties, PR_FID, &sync_data->prop_index.eid);
	SPropTagArray_find(sync_data->properties, PR_PREDECESSOR_CHANGE_LIST, &sync_data->prop_index.precedessor_change_list);
	SPropTagArray_find(sync_data->properties, PR_LAST_MODIFICATION_TIME, &sync_data->prop_index.last_modification_time);
	SPropTagArray_find(sync_data->properties, PR_DISPLAY_NAME_UNICODE, &sync_data->prop_index.display_name);
	sync_data->ndr = ndr_push_init_ctx(sync_data);
	ndr_set_flags(&sync_data->ndr->flags, LIBNDR_FLAG_NOALIGN);
	sync_data->ndr->offset = 0;
	sync_data->cutmarks_ndr = ndr_push_init_ctx(sync_data);
	ndr_set_flags(&sync_data->cutmarks_ndr->flags, LIBNDR_FLAG_NOALIGN);
	sync_data->cutmarks_ndr->offset = 0;
	sync_data->cnset_seen = RAWIDSET_make(sync_data, true);
	sync_data->eid_set = RAWIDSET_make(sync_data, false);

	oxcfxics_push_folderChange(sync_data, emsmdbp_ctx, synccontext, synccontext_object->parent_object, sync_data, synccontext_object->parent_object);

	/* deletions (mapistore v2) */

	/* state */
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_STATE_BEGIN);

	new_idset = RAWIDSET_convert_to_idset(NULL, sync_data->cnset_seen);
	old_idset = synccontext->cnset_seen;
	synccontext->cnset_seen = IDSET_merge_idsets(synccontext, old_idset, new_idset);
	talloc_free(old_idset);
	talloc_free(new_idset);

	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PidTagCnsetSeen);
	ndr_push_idset(sync_data->ndr, synccontext->cnset_seen);

	new_idset = RAWIDSET_convert_to_idset(NULL, sync_data->eid_set);
	old_idset = synccontext->idset_given;
	synccontext->idset_given = IDSET_merge_idsets(synccontext, old_idset, new_idset);
	talloc_free(old_idset);
	talloc_free(new_idset);

	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PidTagIdsetGiven);
	ndr_push_idset(sync_data->ndr, synccontext->idset_given);
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_STATE_END);

	/* end of stream */
	ndr_push_uint32(sync_data->ndr, NDR_SCALARS, PR_INCR_SYNC_END);
	ndr_push_uint32(sync_data->cutmarks_ndr, NDR_SCALARS, 0xffffffff);

	(void) talloc_reference(synccontext, sync_data->ndr->data);
	(void) talloc_reference(synccontext, sync_data->cutmarks_ndr->data);

	synccontext->cutmarks = (uint32_t *) sync_data->cutmarks_ndr->data;
	synccontext->stream.buffer.data = sync_data->ndr->data;
	synccontext->stream.buffer.length = sync_data->ndr->offset;

	talloc_free(sync_data);
}

/**
   \details EcDoRpc EcDoRpc_RopFastTransferSourceGetBuffer (0x4e) Rop. This operation downloads the next portion of a FastTransfer stream that is produced by a previously configured download operation.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the FastTransferSourceGetBuffer EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the FastTransferSourceGetBuffer EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopFastTransferSourceGetBuffer(TALLOC_CTX *mem_ctx,
								struct emsmdbp_context *emsmdbp_ctx,
								struct EcDoRpc_MAPI_REQ *mapi_req,
								struct EcDoRpc_MAPI_REPL *mapi_repl,
								uint32_t *handles, uint16_t *size)
{
	enum MAPISTATUS				retval;
	uint32_t				handle_id;
	struct mapi_handles			*object_handle = NULL;
	struct emsmdbp_object			*object = NULL;
	struct FastTransferSourceGetBuffer_req	 *request;
	struct FastTransferSourceGetBuffer_repl	 *response;
	uint16_t				buffer_size, cutbuffer_size, cutbuffer_pos, mark_ptr, max_cutmark;
	void					*data;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] FastTransferSourceGetBuffer (0x4e)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	/* Step 1. Retrieve object handle */
	handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, handle_id, &object_handle);
	if (retval) {
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		DEBUG(5, ("  handle (%x) not found: %x\n", handle_id, mapi_req->handle_idx));
		goto end;
	}

	/* Step 2. Check whether the parent object supports fetching properties */
	mapi_handles_get_private_data(object_handle, &data);
	object = (struct emsmdbp_object *) data;
	if (!object) {
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		DEBUG(5, ("  object not found\n"));
                goto end;
	}

	request = &mapi_req->u.mapi_FastTransferSourceGetBuffer;
	response = &mapi_repl->u.mapi_FastTransferSourceGetBuffer;

	buffer_size = request->BufferSize;
	if (buffer_size == 0xBABE) {
		buffer_size = request->MaximumBufferSize.MaximumBufferSize;
	}

	/* Step 3. Perform the read operation */
	switch (object->type) {
	case EMSMDBP_OBJECT_FTCONTEXT:
		if (object->object.ftcontext->stream.position == 0) {
			object->object.ftcontext->steps = 0;
			object->object.ftcontext->total_steps = (object->object.ftcontext->stream.buffer.length / buffer_size) + 1;
			DEBUG(5, ("fast transfer buffer is %ld bytes long\n", object->object.ftcontext->stream.buffer.length));
		}
		object->object.ftcontext->steps += 1;

		cutbuffer_pos = object->object.ftcontext->stream.position;
		if (cutbuffer_pos + buffer_size < object->object.ftcontext->stream.buffer.length) {
			mark_ptr = object->object.ftcontext->next_cutmark_ptr;
			max_cutmark = object->object.ftcontext->stream.position + buffer_size;
			while (object->object.ftcontext->cutmarks[mark_ptr] != 0xffffffff && object->object.ftcontext->cutmarks[mark_ptr] < max_cutmark) {
				cutbuffer_pos = object->object.ftcontext->cutmarks[mark_ptr];
				mark_ptr++;
			}
			if (object->object.ftcontext->stream.position < cutbuffer_pos) {
				cutbuffer_size = cutbuffer_pos - object->object.ftcontext->stream.position;
				if (cutbuffer_size > 0 && cutbuffer_size < buffer_size) {
					buffer_size = cutbuffer_size;
				}
			}
		}

		response->TransferBuffer = emsmdbp_stream_read_buffer(&object->object.ftcontext->stream, buffer_size);
		response->TotalStepCount = object->object.ftcontext->total_steps;
		if (object->object.ftcontext->stream.position == object->object.ftcontext->stream.buffer.length) {
			response->TransferStatus = TransferStatus_Done;
			response->InProgressCount = response->TotalStepCount;
		}
		else {
			response->TransferStatus = TransferStatus_Partial;
			response->InProgressCount = object->object.ftcontext->steps;
		}
		break;
	case EMSMDBP_OBJECT_SYNCCONTEXT:
		if (!object->object.synccontext->stream.buffer.data) {
			if (object->object.synccontext->request.contents_mode) {
				oxcfxics_prepare_synccontext_with_messageChange(mem_ctx, object);
			}
			else {
				oxcfxics_prepare_synccontext_with_folderChange(object);
			}
			object->object.synccontext->steps = 0;
			object->object.synccontext->total_steps = (object->object.synccontext->stream.buffer.length / buffer_size) + 1;
			DEBUG(5, ("synccontext buffer is %ld bytes long\n", object->object.synccontext->stream.buffer.length));
		}
		object->object.synccontext->steps += 1;

		cutbuffer_pos = object->object.synccontext->stream.position;
		if (cutbuffer_pos + buffer_size < object->object.synccontext->stream.buffer.length) {
			mark_ptr = object->object.synccontext->next_cutmark_ptr;
			max_cutmark = object->object.synccontext->stream.position + buffer_size;
			while (object->object.synccontext->cutmarks[mark_ptr] != 0xffffffff && object->object.synccontext->cutmarks[mark_ptr] < max_cutmark) {
				cutbuffer_pos = object->object.synccontext->cutmarks[mark_ptr];
				mark_ptr++;
			}
			if (object->object.ftcontext->stream.position < cutbuffer_pos) {
				cutbuffer_size = cutbuffer_pos - object->object.synccontext->stream.position;
				if (cutbuffer_size > 0 && cutbuffer_size < buffer_size) {
					buffer_size = cutbuffer_size;
				}
			}
		}

		response->TransferBuffer = emsmdbp_stream_read_buffer(&object->object.synccontext->stream, buffer_size);
		response->TotalStepCount = object->object.synccontext->total_steps;
		if (object->object.synccontext->stream.position == object->object.synccontext->stream.buffer.length) {
			response->TransferStatus = TransferStatus_Done;
			response->InProgressCount = response->TotalStepCount;
		}
		else {
			response->TransferStatus = TransferStatus_Partial;
			response->InProgressCount = object->object.synccontext->steps;
		}
		break;
	default:
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		DEBUG(5, ("  object type %d not supported\n", object->type));
                goto end;
	}

	response->TransferBufferSize = response->TransferBuffer.length;
	response->Reserved = 0;

end:
	*size += libmapiserver_RopFastTransferSourceGetBuffer_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncConfigure (0x70) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncConfigure EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncConfigure EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncConfigure(TALLOC_CTX *mem_ctx,
						  struct emsmdbp_context *emsmdbp_ctx,
						  struct EcDoRpc_MAPI_REQ *mapi_req,
						  struct EcDoRpc_MAPI_REPL *mapi_repl,
						  uint32_t *handles, uint16_t *size)
{
	struct SyncConfigure_req		*request;
	uint32_t				folder_handle;
	struct mapi_handles			*folder_rec;
	struct mapi_handles			*synccontext_rec;
	struct emsmdbp_object			*folder_object;
	struct emsmdbp_object			*synccontext_object;
	struct emsmdbp_object			*table_object;
        struct emsmdbp_object_synccontext	*synccontext;
	enum MAPISTATUS				retval;
	bool					*properties_exclusion;
	bool					include_props;
	uint16_t				i, j;
	void					*data = NULL;
	struct SPropTagArray			*available_properties;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncConfigure (0x70)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	request = &mapi_req->u.mapi_SyncConfigure;

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
        mapi_repl->handle_idx = request->handle_idx;

	folder_handle = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, folder_handle, &folder_rec);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", folder_handle, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		goto end;
	}

	mapi_handles_get_private_data(folder_rec, &data);
	folder_object = (struct emsmdbp_object *)data;
	if (!folder_object || folder_object->type != EMSMDBP_OBJECT_FOLDER) {
		DEBUG(5, ("  object not found or not a folder\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

        synccontext_object = emsmdbp_object_synccontext_init(NULL, emsmdbp_ctx, folder_object);
        synccontext = synccontext_object->object.synccontext;

	/* SynchronizationType */
	synccontext->request.contents_mode = (request->SynchronizationType == Contents);

	/* SendOptions */
	synccontext->request.unicode = (request->SendOptions & FastTransfer_Unicode);
	synccontext->request.use_cpid = (request->SendOptions & FastTransfer_UseCpid);
	synccontext->request.recover_mode = (request->SendOptions & FastTransfer_RecoverMode);
	synccontext->request.force_unicode = (request->SendOptions & FastTransfer_ForceUnicode);
	synccontext->request.partial_item = (request->SendOptions & FastTransfer_PartialItem);

	/* SynchronizationFlag */
	if (synccontext->request.unicode && !(request->SynchronizationFlag & SynchronizationFlag_Unicode)) {
		DEBUG(4, ("unhandled value for SynchronizationType: %d\n", request->SynchronizationType));
		mapi_repl->error_code = MAPI_E_INVALID_PARAMETER;
		talloc_free(synccontext_object);
		goto end;
	}
	synccontext->request.no_deletions = (request->SynchronizationFlag & SynchronizationFlag_NoDeletions);
	synccontext->request.no_soft_deletions = (request->SynchronizationFlag & SynchronizationFlag_NoSoftDeletions);
	synccontext->request.ignore_no_longer_in_scope = (request->SynchronizationFlag & SynchronizationFlag_NoSoftDeletions);
	synccontext->request.read_state = (request->SynchronizationFlag & SynchronizationFlag_ReadState);
	synccontext->request.fai = (request->SynchronizationFlag & SynchronizationFlag_FAI);
	synccontext->request.normal = (request->SynchronizationFlag & SynchronizationFlag_Normal);
	synccontext->request.no_foreign_identifiers = (request->SynchronizationFlag & SynchronizationFlag_NoForeignIdentifiers);
	synccontext->request.best_body = (request->SynchronizationFlag & SynchronizationFlag_BestBody);
	synccontext->request.ignored_specified_on_fai = (request->SynchronizationFlag & SynchronizationFlag_IgnoreSpecifiedOnFAI);
	synccontext->request.progress = (request->SynchronizationFlag & SynchronizationFlag_Progress);

	/* SynchronizationExtraFlag */
	synccontext->request.request_eid = (request->SynchronizationExtraFlags & Eid);
	synccontext->request.request_message_size = (request->SynchronizationExtraFlags & MessageSize);
	synccontext->request.request_cn = (request->SynchronizationExtraFlags & Cn);
	synccontext->request.order_by_delivery_time = (request->SynchronizationExtraFlags & OrderByDeliveryTime);

	/* Building the real properties array... */
	properties_exclusion = talloc_array(NULL, bool, 65536);
	memset(properties_exclusion, 0, 65536 * sizeof(bool));

	synccontext->properties.cValues = 0;
	synccontext->properties.aulPropTag = talloc_zero(synccontext, enum MAPITAGS);
	if (synccontext->request.contents_mode) {	/* keyword: messageChangeHeader */
		SPropTagArray_add(synccontext, &synccontext->properties, PR_MID); /* PR_SOURCE_KEY */
		SPropTagArray_add(synccontext, &synccontext->properties, PR_ASSOCIATED);
		SPropTagArray_add(synccontext, &synccontext->properties, PR_MESSAGE_SIZE);
		SPropTagArray_add(synccontext, &synccontext->properties, PR_CHANGE_NUM);
	}
	else {						/* keyword: folderChange */
		SPropTagArray_add(synccontext, &synccontext->properties, PR_PARENT_FID); /* PR_PARENT_SOURCE_KEY */
		SPropTagArray_add(synccontext, &synccontext->properties, PR_FID); /* PR_SOURCE_KEY */
	}
	SPropTagArray_add(synccontext, &synccontext->properties, PR_CHANGE_KEY);
	SPropTagArray_add(synccontext, &synccontext->properties, PR_LAST_MODIFICATION_TIME);
	SPropTagArray_add(synccontext, &synccontext->properties, PR_DISPLAY_NAME_UNICODE);
	for (j = 0; j < synccontext->properties.cValues; j++) {
		i = (synccontext->properties.aulPropTag[j] & 0xffff0000) >> 16;
		properties_exclusion[i] = true;
	}

	/* Explicit exclusions */
	properties_exclusion[PR_ROW_TYPE >> 16] = true;
	properties_exclusion[PR_INSTANCE_KEY >> 16] = true;
	properties_exclusion[PR_INSTANCE_NUM >> 16] = true;
	properties_exclusion[PR_INST_ID >> 16] = true;
	properties_exclusion[PR_FID >> 16] = true;
	properties_exclusion[PR_MID >> 16] = true;
	properties_exclusion[PR_SOURCE_KEY >> 16] = true;
	properties_exclusion[PR_PARENT_SOURCE_KEY >> 16] = true;
	properties_exclusion[PR_PARENT_FID >> 16] = true;

	/* Include or exclude specified properties passed in array */
	include_props = ((request->SynchronizationFlag & SynchronizationFlag_OnlySpecifiedProperties));
	for (j = 0; j < request->PropertyTags.cValues; j++) {
		i = (request->PropertyTags.aulPropTag[j] & 0xffff0000) >> 16;
		if (!properties_exclusion[i]) {
			properties_exclusion[i] = true; /* avoid including the same prop twice */
			if (include_props) {
				SPropTagArray_add(synccontext, &synccontext->properties, request->PropertyTags.aulPropTag[j]);
			}
		}
	}

	/* When "best body" is requested and one of the required properties is excluded, we include it back */
	if (!include_props && ((request->SynchronizationFlag & SynchronizationFlag_BestBody))) {
		properties_exclusion[PR_BODY_HTML >> 16] = false;
		properties_exclusion[PR_BODY_UNICODE >> 16] = false;
	}

	/* we instantiate a table object that will help us retrieve the list of available properties */
	if (!include_props) {
		if (synccontext->request.contents_mode) {
			if (synccontext->request.normal) {
				table_object = emsmdbp_folder_open_table(NULL, folder_object, EMSMDBP_TABLE_MESSAGE_TYPE, 0);
				if (!table_object) {
					DEBUG(5, ("could not open folder table\n"));
					abort();
				}
				if (emsmdbp_object_table_get_available_properties(mem_ctx, emsmdbp_ctx, table_object, &available_properties) == MAPISTORE_SUCCESS) {
					for (j = 0; j < available_properties->cValues; j++) {
						i = (available_properties->aulPropTag[j] & 0xffff0000) >> 16;
						if (!properties_exclusion[i]) {
							properties_exclusion[i] = true;
							SPropTagArray_add(synccontext, &synccontext->properties, available_properties->aulPropTag[j]);
						}
					}
					talloc_free(available_properties->aulPropTag);
					talloc_free(available_properties);
				}
				talloc_free(table_object);
			}

			if (synccontext->request.fai) {
				table_object = emsmdbp_folder_open_table(NULL, folder_object, EMSMDBP_TABLE_FAI_TYPE, 0);
				if (!table_object) {
					DEBUG(5, ("could not open folder table\n"));
					abort();
				}
				if (emsmdbp_object_table_get_available_properties(mem_ctx, emsmdbp_ctx, table_object, &available_properties) == MAPISTORE_SUCCESS) {
					for (j = 0; j < available_properties->cValues; j++) {
						i = (available_properties->aulPropTag[j] & 0xffff0000) >> 16;
						if (!properties_exclusion[i]) {
							properties_exclusion[i] = true;
							SPropTagArray_add(synccontext, &synccontext->properties, available_properties->aulPropTag[j]);
						}
					}
					talloc_free(available_properties->aulPropTag);
					talloc_free(available_properties);
				}
				talloc_free(table_object);
			}
		}
		else {
			table_object = emsmdbp_folder_open_table(NULL, folder_object, EMSMDBP_TABLE_FOLDER_TYPE, 0);
			if (!table_object) {
				DEBUG(5, ("could not open folder table\n"));
				abort();
			}
			if (emsmdbp_object_table_get_available_properties(mem_ctx, emsmdbp_ctx, table_object, &available_properties) == MAPISTORE_SUCCESS) {
				for (j = 0; j < available_properties->cValues; j++) {
					i = (available_properties->aulPropTag[j] & 0xffff0000) >> 16;
					if (!properties_exclusion[i]) {
						properties_exclusion[i] = true;
						SPropTagArray_add(synccontext, &synccontext->properties, available_properties->aulPropTag[j]);
					}
				}
				talloc_free(available_properties->aulPropTag);
				talloc_free(available_properties);
			}
			talloc_free(table_object);
		}
	}
	talloc_free(properties_exclusion);

	/* TODO: handle restrictions */

	/* The properties array is now ready and further processing must occur in the first FastTransferSource_GetBuffer since we need to wait to receive the state streams in order to build it. */

        retval = mapi_handles_add(emsmdbp_ctx->handles_ctx, folder_handle, &synccontext_rec);
	(void) talloc_reference(synccontext_rec, synccontext_object);
        mapi_handles_set_private_data(synccontext_rec, synccontext_object);
	talloc_free(synccontext_object);
        handles[mapi_repl->handle_idx] = synccontext_rec->handle;
end:
	*size += libmapiserver_RopSyncConfigure_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncImportMessageChange (0x72) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncImportMessageChange EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncImportMessageChange EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncImportMessageChange(TALLOC_CTX *mem_ctx,
							    struct emsmdbp_context *emsmdbp_ctx,
							    struct EcDoRpc_MAPI_REQ *mapi_req,
							    struct EcDoRpc_MAPI_REPL *mapi_repl,
							    uint32_t *handles, uint16_t *size)
{
	enum MAPISTATUS				retval;
	struct mapi_handles			*synccontext_object_handle = NULL, *message_object_handle;
	struct emsmdbp_object			*synccontext_object = NULL, *message_object;
	uint32_t				synccontext_handle_id, message_handle_id;
	void					*data;
	struct SyncImportMessageChange_req	*request;
	struct SyncImportMessageChange_repl	*response;
	uint64_t				folderID;
	uint64_t				messageID;
	struct GUID				replica_guid;
	uint16_t				repl_id;
	struct mapistore_message		*msg;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncImportMessageChange (0x72)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	request = &mapi_req->u.mapi_SyncImportMessageChange;
	response = &mapi_repl->u.mapi_SyncImportMessageChange;

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = request->handle_idx;

	/* Step 1. Retrieve object handle */
	synccontext_handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle_id, &synccontext_object_handle);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle_id, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(synccontext_object_handle, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	if (!emsmdbp_is_mapistore(synccontext_object->parent_object)) {
		DEBUG(5, ("  cannot create message on non-mapistore object\n"));
		mapi_repl->error_code = MAPI_E_NO_SUPPORT;
		goto end;
	}

	openchangedb_get_MailboxReplica(emsmdbp_ctx->oc_ctx, emsmdbp_ctx->username, &repl_id, &replica_guid);
	if (oxcfxics_fmid_from_source_key(emsmdbp_ctx, &request->PropertyValues.lpProps[0].value.bin, &messageID)) {
		mapi_repl->error_code = MAPI_E_NOT_FOUND;
		goto end;
	}

	folderID = synccontext_object->parent_object->object.folder->folderID;

	/* Initialize Message object */
	message_handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_add(emsmdbp_ctx->handles_ctx, message_handle_id, &message_object_handle);
	handles[mapi_repl->handle_idx] = message_object_handle->handle;

	message_object = emsmdbp_object_message_open(message_object_handle, emsmdbp_ctx, synccontext_object->parent_object, folderID, messageID, &msg);
	if (!message_object) {
		message_object = emsmdbp_object_message_init(message_object_handle, emsmdbp_ctx, messageID, synccontext_object->parent_object);
		if (mapistore_folder_create_message(emsmdbp_ctx->mstore_ctx, emsmdbp_get_contextID(synccontext_object->parent_object), synccontext_object->parent_object->backend_object, message_object, messageID, (request->ImportFlag & ImportFlag_Associated), &message_object->backend_object)) {
			mapi_handles_delete(emsmdbp_ctx->handles_ctx, message_object_handle->handle);
			DEBUG(5, ("could not open nor create mapistore message\n"));
			mapi_repl->error_code = MAPI_E_NOT_FOUND;
			goto end;
		}
	}
	mapi_handles_set_private_data(message_object_handle, message_object);

	response->MessageId = 0; /* Must be set to 0 */

end:
	*size += libmapiserver_RopSyncImportMessageChange_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncImportHierarchyChange (0x73) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncImportHierarchyChange EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncImportHierarchyChange EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncImportHierarchyChange(TALLOC_CTX *mem_ctx,
							      struct emsmdbp_context *emsmdbp_ctx,
							      struct EcDoRpc_MAPI_REQ *mapi_req,
							      struct EcDoRpc_MAPI_REPL *mapi_repl,
							      uint32_t *handles, uint16_t *size)
{
	enum MAPISTATUS				retval;
	struct mapi_handles			*synccontext_object_handle = NULL;
	struct emsmdbp_object			*synccontext_object = NULL, *folder_object = NULL, *parent_folder = NULL;
	uint32_t				synccontext_handle_id;
	void					*data;
	struct SyncImportHierarchyChange_req	*request;
	struct SyncImportHierarchyChange_repl	*response;
	uint64_t				parentFolderID;
	uint64_t				folderID;
	struct GUID				replica_guid;
	uint16_t				repl_id;
	uint32_t				i;
	struct SRow				aRow;
	bool					folder_was_open = true;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncImportHierarchyChange (0x73)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	/* Step 1. Retrieve object handle */
	synccontext_handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle_id, &synccontext_object_handle);
	if (retval) {
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle_id, mapi_req->handle_idx));
		goto end;
	}

	mapi_handles_get_private_data(synccontext_object_handle, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	request = &mapi_req->u.mapi_SyncImportHierarchyChange;
	response = &mapi_repl->u.mapi_SyncImportHierarchyChange;

	openchangedb_get_MailboxReplica(emsmdbp_ctx->oc_ctx, emsmdbp_ctx->username, &repl_id, &replica_guid);

	/* deduce the parent folder id (fixed position 0). */
	if (oxcfxics_fmid_from_source_key(emsmdbp_ctx, &request->HierarchyValues.lpProps[0].value.bin, &parentFolderID)) {
		mapi_repl->error_code = MAPI_E_NOT_FOUND;
		goto end;
	}

	/* deduce the folder id (fixed position 1) */
	if (oxcfxics_fmid_from_source_key(emsmdbp_ctx, &request->HierarchyValues.lpProps[1].value.bin, &folderID)) {
		mapi_repl->error_code = MAPI_E_NOT_FOUND;
		goto end;
	}

	aRow.cValues = request->HierarchyValues.cValues + request->PropertyValues.cValues;
	aRow.lpProps = talloc_array(mem_ctx, struct SPropValue, aRow.cValues + 2);
	for (i = 0; i < request->HierarchyValues.cValues; i++) {
		cast_SPropValue(aRow.lpProps, request->HierarchyValues.lpProps + i, aRow.lpProps + i);
	}
	for (i = 0; i < request->PropertyValues.cValues; i++) {
		cast_SPropValue(aRow.lpProps, request->PropertyValues.lpProps + i, aRow.lpProps + request->HierarchyValues.cValues + i);
	}

	/* Initialize folder object */
	if (synccontext_object->parent_object->object.folder->folderID == parentFolderID) {
		parent_folder = synccontext_object->parent_object;
		folder_was_open = true;
	}
	else {
		parent_folder = emsmdbp_object_open_folder_by_fid(NULL, emsmdbp_ctx, synccontext_object->parent_object, parentFolderID);
		folder_was_open = false;
	}

	folder_object = emsmdbp_object_open_folder(NULL, emsmdbp_ctx, parent_folder, folderID);
	if (!folder_object) {
		retval = emsmdbp_object_create_folder(emsmdbp_ctx, parent_folder, NULL, folderID, &aRow, &folder_object);
		if (retval) {
			folder_object = NULL;
			mapi_repl->error_code = MAPI_E_NO_SUPPORT;
			goto end;
		}
	}

	/* Set properties on folder object */
	retval = emsmdbp_object_set_properties(emsmdbp_ctx, folder_object, &aRow);
	if (retval) {
		mapi_repl->error_code = MAPI_E_NO_SUPPORT;
		goto end;
	}
	response->FolderId = 0; /* Must be set to 0 */

end:
	if (folder_object) {
		talloc_free(folder_object);
	}
	if (!folder_was_open) {
		talloc_free(parent_folder);
	}

	*size += libmapiserver_RopSyncImportHierarchyChange_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc SyncImportDeletes (0x74) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncImportDeletes EcDoRpc_MAPI_REQ
   \param mapi_repl pointer to the SyncImportDeletes EcDoRpc_MAPI_REPL
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncImportDeletes(TALLOC_CTX *mem_ctx,
						      struct emsmdbp_context *emsmdbp_ctx,
						      struct EcDoRpc_MAPI_REQ *mapi_req,
						      struct EcDoRpc_MAPI_REPL *mapi_repl,
						      uint32_t *handles, uint16_t *size)
{
	enum MAPISTATUS				retval;
	struct mapi_handles			*synccontext_object_handle = NULL;
	struct emsmdbp_object			*synccontext_object = NULL;
	uint32_t				synccontext_handle_id;
	void					*data;
	struct SyncImportDeletes_req		*request;
	uint32_t				contextID;
	uint64_t				objectID;
	struct GUID				replica_guid;
	uint16_t				repl_id;
	struct mapi_SBinaryArray		*object_array;
	uint8_t					delete_type;
	uint32_t				i;
	int					ret;					

	DEBUG(4, ("exchange_emsmdb: [OXCSTOR] SyncImportDeletes (0x74)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	/* Step 1. Retrieve object handle */
	synccontext_handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle_id, &synccontext_object_handle);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle_id, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(synccontext_object_handle, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	request = &mapi_req->u.mapi_SyncImportDeletes;

	if (request->Flags & SyncImportDeletes_Hierarchy) {
		DEBUG(5, ("  hierarchy deletes not supported yet\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	if (request->Flags & SyncImportDeletes_HardDelete) {
		delete_type = MAPISTORE_PERMANENT_DELETE;
	}
	else {
		delete_type = MAPISTORE_SOFT_DELETE;
	}

	if (!emsmdbp_is_mapistore(synccontext_object)) {
		DEBUG(5, ("  no message deletes on non-mapistore store\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	contextID = emsmdbp_get_contextID(synccontext_object);
	openchangedb_get_MailboxReplica(emsmdbp_ctx->oc_ctx, emsmdbp_ctx->username, &repl_id, &replica_guid);

	object_array = &request->PropertyValues.lpProps[0].value.MVbin;
	for (i = 0; i < object_array->cValues; i++) {
		ret = oxcfxics_fmid_from_source_key(emsmdbp_ctx, object_array->bin + i, &objectID);
		if (ret == MAPISTORE_SUCCESS) {
			ret = mapistore_folder_delete_message(emsmdbp_ctx->mstore_ctx, contextID, synccontext_object->parent_object->backend_object, objectID, delete_type);
			if (ret != MAPISTORE_SUCCESS) {
				DEBUG(5, ("message deletion failed for fmid: 0x%.16"PRIx64"\n", objectID));
			}
			ret = mapistore_indexing_record_del_mid(emsmdbp_ctx->mstore_ctx, contextID, objectID, delete_type);
			if (ret != MAPISTORE_SUCCESS) {
				DEBUG(5, ("message deletion of index record failed for fmid: 0x%.16"PRIx64"\n", objectID));
			}
		}
	}

end:
	*size += libmapiserver_RopSyncImportDeletes_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncUploadStateStreamBegin (0x75) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncUploadStateStreamBegin EcDoRpc_MAPI_REQ
   structure
   \param mapi_repl pointer to the SyncUploadStateStreamBegin EcDoRpc_MAPI_REPL
   structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncUploadStateStreamBegin(TALLOC_CTX *mem_ctx,
							       struct emsmdbp_context *emsmdbp_ctx,
							       struct EcDoRpc_MAPI_REQ *mapi_req,
							       struct EcDoRpc_MAPI_REPL *mapi_repl,
							       uint32_t *handles, uint16_t *size)
{
 	uint32_t		synccontext_handle;
	struct mapi_handles	*synccontext_rec;
	struct emsmdbp_object	*synccontext_object;
	enum MAPISTATUS		retval;
	enum StateProperty	property;
	void			*data = NULL;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncUploadStateStreamBegin (0x75)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	synccontext_handle = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle, &synccontext_rec);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(synccontext_rec, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		goto end;
	}

	if (synccontext_object->object.synccontext->state_property != 0) {
		DEBUG(5, ("  stream already in pending state\n"));
		mapi_repl->error_code = MAPI_E_NOT_INITIALIZED;
		goto end;
	}

	property = mapi_req->u.mapi_SyncUploadStateStreamBegin.StateProperty;
	if (!(property == PidTagIdsetGiven || property == PidTagCnsetSeen || property == PidTagCnsetSeenFAI || property == PidTagCnsetRead)) {
		DEBUG(5, ("  state property is invalid\n"));
		mapi_repl->error_code = MAPI_E_INVALID_PARAMETER;
		goto end;
	}

	synccontext_object->object.synccontext->state_property = property;
	memset(&synccontext_object->object.synccontext->state_stream, 0, sizeof(struct emsmdbp_stream));
	synccontext_object->object.synccontext->state_stream.buffer.data = talloc_zero(synccontext_object->object.synccontext, uint8_t);

end:
	*size += libmapiserver_RopSyncUploadStateStreamBegin_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncUploadStateStreamContinue (0x76) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncUploadStateStreamContinue EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncUploadStateStreamContinue EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncUploadStateStreamContinue(TALLOC_CTX *mem_ctx,
								  struct emsmdbp_context *emsmdbp_ctx,
								  struct EcDoRpc_MAPI_REQ *mapi_req,
								  struct EcDoRpc_MAPI_REPL *mapi_repl,
								  uint32_t *handles, uint16_t *size)
{
 	uint32_t		synccontext_handle;
	struct mapi_handles	*synccontext_rec;
	struct emsmdbp_object	*synccontext_object;
	enum MAPISTATUS		retval;
	void			*data = NULL;
	struct SyncUploadStateStreamContinue_req *request;
	DATA_BLOB		new_data;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncUploadStateStreamContinue (0x76)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	synccontext_handle = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle, &synccontext_rec);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(synccontext_rec, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		goto end;
	}

	if (synccontext_object->object.synccontext->state_property == 0) {
		DEBUG(5, ("  attempt to feed an idle stream\n"));
		mapi_repl->error_code = MAPI_E_NOT_INITIALIZED;
		goto end;
	}

	request = &mapi_req->u.mapi_SyncUploadStateStreamContinue;
	new_data.length = request->StreamDataSize;
	new_data.data = request->StreamData;
	emsmdbp_stream_write_buffer(synccontext_object->object.synccontext,
				    &synccontext_object->object.synccontext->state_stream,
				    new_data);

end:
	*size += libmapiserver_RopSyncUploadStateStreamContinue_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncUploadStateStreamEnd (0x77) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncUploadStateStreamEnd EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncUploadStateStreamEnd EcDoRpc_MAPI_REPL  structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncUploadStateStreamEnd(TALLOC_CTX *mem_ctx,
							     struct emsmdbp_context *emsmdbp_ctx,
							     struct EcDoRpc_MAPI_REQ *mapi_req,
							     struct EcDoRpc_MAPI_REPL *mapi_repl,
							     uint32_t *handles, uint16_t *size)
{
 	uint32_t				synccontext_handle;
	struct mapi_handles			*synccontext_rec;
	struct emsmdbp_object			*synccontext_object;
	struct emsmdbp_object_synccontext	*synccontext;
	struct idset				*parsed_idset, *old_idset;
	enum MAPISTATUS				retval;
	void					*data = NULL;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncUploadStateStreamEnd (0x77)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	synccontext_handle = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle, &synccontext_rec);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(synccontext_rec, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		goto end;
	}

	if (synccontext_object->object.synccontext->state_property == 0) {
		DEBUG(5, ("  attempt to end an idle stream\n"));
		mapi_repl->error_code = MAPI_E_NOT_INITIALIZED;
		goto end;
	}

	if (synccontext_object->object.synccontext->is_collector) {
		DEBUG(5, ("  synccontext is collector\n"));
	}

	/* parse IDSET */
	synccontext = synccontext_object->object.synccontext;
	parsed_idset = IDSET_parse(synccontext, synccontext->state_stream.buffer);

	switch (synccontext->state_property) {
	case PidTagIdsetGiven:
		if (parsed_idset && parsed_idset->range_count == 0) {
			abort();
		}
		old_idset = synccontext->idset_given;
		if (old_idset) {
			talloc_free(old_idset);
		}
		synccontext->idset_given = parsed_idset;
		break;
	case PidTagCnsetSeen:
	case PidTagCnsetSeenFAI:
	case PidTagCnsetRead:
		if (parsed_idset) {
			parsed_idset->single = true;
		}
		old_idset = synccontext->cnset_seen;
		if (old_idset) {
			talloc_free(old_idset);
		}
		synccontext->cnset_seen = parsed_idset;
		break;
	}

	/* reset synccontext state */
	if (synccontext->state_stream.buffer.length > 0) {
		talloc_free(synccontext->state_stream.buffer.data);
		synccontext->state_stream.buffer.data = talloc_zero(synccontext, uint8_t);
		synccontext->state_stream.buffer.length = 0;
	}

	synccontext->state_property = 0;

end:
	*size += libmapiserver_RopSyncUploadStateStreamEnd_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc SyncImportMessageMove (0x78) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncImportMessageMove EcDoRpc_MAPI_REQ
   \param mapi_repl pointer to the SyncImportMessageMove EcDoRpc_MAPI_REPL
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncImportMessageMove(TALLOC_CTX *mem_ctx,
							  struct emsmdbp_context *emsmdbp_ctx,
							  struct EcDoRpc_MAPI_REQ *mapi_req,
							  struct EcDoRpc_MAPI_REPL *mapi_repl,
							  uint32_t *handles, uint16_t *size)
{
	struct SyncImportMessageMove_repl	*response;

	DEBUG(4, ("exchange_emsmdb: [OXCSTOR] SyncImportMessageMove (0x78) - stub\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->handle_idx = mapi_req->handle_idx;
	mapi_repl->error_code = MAPI_E_SUCCESS;

	/* TODO effective work here */
	response = &mapi_repl->u.mapi_SyncImportMessageMove;
	response->MessageId = 0;

	*size += libmapiserver_RopSyncImportMessageMove_size(mapi_repl);

	handles[mapi_repl->handle_idx] = handles[mapi_req->handle_idx];

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopSyncOpenCollector (0x7e) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncOpenCollector EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncOpenCollector EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncOpenCollector(TALLOC_CTX *mem_ctx,
						      struct emsmdbp_context *emsmdbp_ctx,
						      struct EcDoRpc_MAPI_REQ *mapi_req,
						      struct EcDoRpc_MAPI_REPL *mapi_repl,
						      uint32_t *handles, uint16_t *size)
{
	uint32_t		folder_handle;
	struct mapi_handles	*folder_rec;
	struct mapi_handles	*synccontext_rec;
	struct emsmdbp_object	*folder_object;
	struct emsmdbp_object	*synccontext_object;
	enum MAPISTATUS		retval;
	void			*data = NULL;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncOpenCollector (0x7e)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->u.mapi_SyncOpenCollector.handle_idx;

	folder_handle = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, folder_handle, &folder_rec);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", folder_handle, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(folder_rec, &data);
	folder_object = (struct emsmdbp_object *)data;
	if (!folder_object || folder_object->type != EMSMDBP_OBJECT_FOLDER) {
		DEBUG(5, ("  object not found or not a folder\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		goto end;
	}

	retval = mapi_handles_add(emsmdbp_ctx->handles_ctx, folder_handle, &synccontext_rec);

	synccontext_object = emsmdbp_object_synccontext_init((TALLOC_CTX *)synccontext_rec, emsmdbp_ctx, folder_object);
	synccontext_object->object.synccontext->is_collector = true;

	talloc_steal(synccontext_rec, synccontext_object);
	retval = mapi_handles_set_private_data(synccontext_rec, synccontext_object);
	synccontext_object->object.synccontext->request.contents_mode = (mapi_req->u.mapi_SyncOpenCollector.IsContentsCollector != 0);
	handles[mapi_repl->handle_idx] = synccontext_rec->handle;

end:
	*size += libmapiserver_RopSyncOpenCollector_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc EcDoRpc_RopGetLocalReplicaIds (0x7f) Rop. This operation reserves a range of IDs to be used by a local replica.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the GetLocalReplicaIds EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the GetLocalReplicaIds EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopGetLocalReplicaIds(TALLOC_CTX *mem_ctx,
                                                       struct emsmdbp_context *emsmdbp_ctx,
                                                       struct EcDoRpc_MAPI_REQ *mapi_req,
                                                       struct EcDoRpc_MAPI_REPL *mapi_repl,
                                                       uint32_t *handles, uint16_t *size)
{
	struct GetLocalReplicaIds_req	*request;
	struct mapi_handles		*object_handle;
	uint32_t			handle_id;
	uint64_t			new_id;
	uint8_t				i;
	void				*data;
	int				retval;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopGetLocalReplicaIds (0x7f)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->handle_idx;

	/* Step 1. Retrieve object handle */
	handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, handle_id, &object_handle);
	if (retval) {
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		DEBUG(5, ("  handle (%x) not found: %x\n", handle_id, mapi_req->handle_idx));
		goto end;
	}

	/* Step 2. Check whether the parent object supports fetching properties */
	mapi_handles_get_private_data(object_handle, &data);

	request = &mapi_req->u.mapi_GetLocalReplicaIds;

	emsmdbp_replid_to_guid(emsmdbp_ctx, 0x0001, &mapi_repl->u.mapi_GetLocalReplicaIds.ReplGuid);
	openchangedb_reserve_fmid_range(emsmdbp_ctx->oc_ctx, request->IdCount, &new_id);
	new_id >>= 16;
	for (i = 0; i < 6 ; i++) {
		mapi_repl->u.mapi_GetLocalReplicaIds.GlobalCount[i] = new_id & 0xff;
		new_id >>= 8;
	}

end:
	*size += libmapiserver_RopGetLocalReplicaIds_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc SyncImportReadStateChanges (0x80) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncImportReadStateChanges EcDoRpc_MAPI_REQ
   \param mapi_repl pointer to the SyncImportReadStateChanges EcDoRpc_MAPI_REPL
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncImportReadStateChanges(TALLOC_CTX *mem_ctx,
							       struct emsmdbp_context *emsmdbp_ctx,
							       struct EcDoRpc_MAPI_REQ *mapi_req,
							       struct EcDoRpc_MAPI_REPL *mapi_repl,
							       uint32_t *handles, uint16_t *size)
{
	DEBUG(4, ("exchange_emsmdb: [OXCSTOR] SyncImportReadStateChanges (0x80) - stub\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->handle_idx = mapi_req->handle_idx;
	mapi_repl->error_code = MAPI_E_SUCCESS;

	/* TODO effective work here */

	*size += libmapiserver_RopSyncImportReadStateChanges_size(mapi_repl);

	handles[mapi_repl->handle_idx] = handles[mapi_req->handle_idx];

	return MAPI_E_SUCCESS;
}

static void oxcfxics_fill_transfer_state_arrays(TALLOC_CTX *mem_ctx, struct emsmdbp_context *emsmdbp_ctx, struct emsmdbp_object_synccontext *synccontext, struct oxcfxics_sync_data *sync_data, struct emsmdbp_object *folder_object)
{
	struct SPropTagArray		*count_query_props;
	uint64_t			eid, cn;
	uint32_t			i, nr_eid;
	void				**data_pointers;
	uint32_t			*retvals;
	struct emsmdbp_object		*table_object, *subfolder_object;
	struct emsmdbp_object_table	*table;
	uint32_t			unix_time;
	struct FILETIME			*lm_time;
	NTTIME				nt_time;
	void				*local_mem_ctx;
	struct GUID			replica_guid;
	
	local_mem_ctx = talloc_zero(NULL, void);

	/* Query the amount of rows and update sync_data structure */
	count_query_props = talloc_zero(local_mem_ctx, struct SPropTagArray);
	count_query_props->cValues = 1;
	count_query_props->aulPropTag = talloc_zero(count_query_props, enum MAPITAGS);
	switch (sync_data->table_type) {
	case EMSMDBP_TABLE_FOLDER_TYPE:
		count_query_props->aulPropTag[0] = PR_FOLDER_CHILD_COUNT;
		break;
	case EMSMDBP_TABLE_MESSAGE_TYPE:
		count_query_props->aulPropTag[0] = PR_CONTENT_COUNT;
		break;
	case EMSMDBP_TABLE_FAI_TYPE:
		count_query_props->aulPropTag[0] = PR_ASSOC_CONTENT_COUNT;
		break;
	default:
		abort();
	}
	data_pointers = emsmdbp_object_get_properties(local_mem_ctx, emsmdbp_ctx, folder_object, count_query_props, (enum MAPISTATUS **) &retvals);
	if (data_pointers && !retvals[0]) {
		nr_eid = *(uint32_t *) data_pointers[0];
	}
	else {
		DEBUG(5, ("could not retrieve number of rows in table\n"));
		abort();
	}

	if (!nr_eid) {
		return;
	}

	/* Fetch the actual table data */
 	table_object = emsmdbp_folder_open_table(local_mem_ctx, folder_object, sync_data->table_type, 0); 
	if (!table_object) {
		DEBUG(5, ("could not open folder table\n"));
		abort();
	}
	table_object->object.table->prop_count = sync_data->properties.cValues;
	table_object->object.table->properties = sync_data->properties.aulPropTag;
	if (emsmdbp_is_mapistore(table_object)) {
		mapistore_table_set_columns(emsmdbp_ctx->mstore_ctx, emsmdbp_get_contextID(table_object), table_object->backend_object, sync_data->properties.cValues, sync_data->properties.aulPropTag);
	}
	table = table_object->object.table;
	for (i = 0; i < table->denominator; i++) {
		data_pointers = emsmdbp_object_table_get_row_props(NULL, emsmdbp_ctx, table_object, i, &retvals);
		if (data_pointers) {
			eid = *(uint64_t *) data_pointers[0];
			emsmdbp_replid_to_guid(emsmdbp_ctx, eid & 0xffff, &replica_guid);
			RAWIDSET_push_glob(sync_data->eid_set, &replica_guid, eid >> 16);
			
			if (retvals[1]) {
				unix_time = oc_version_time;
			}
			else {
				lm_time = (struct FILETIME *) data_pointers[1];
				nt_time = ((uint64_t) lm_time->dwHighDateTime << 32) | lm_time->dwLowDateTime;
				unix_time = nt_time_to_unix(nt_time);
			}

			if (unix_time < oc_version_time) {
				unix_time = oc_version_time;
			}
			cn = ((eid & 0xffff000000000000LL) >> 16) | (exchange_globcnt(unix_time - oc_version_time) >> 16);
			RAWIDSET_push_glob(sync_data->cnset_seen, &sync_data->replica_guid, cn);
			
			talloc_free(retvals);
			talloc_free(data_pointers);

			if (sync_data->table_type == EMSMDBP_TABLE_FOLDER_TYPE) {
				subfolder_object = emsmdbp_object_open_folder(local_mem_ctx, emsmdbp_ctx, folder_object, eid);
				oxcfxics_fill_transfer_state_arrays(mem_ctx, emsmdbp_ctx, synccontext, sync_data, subfolder_object);
				talloc_free(subfolder_object);
			}
		}
	}

	talloc_free(local_mem_ctx);
}

static void oxcfxics_ndr_push_transfer_state(struct ndr_push *ndr, struct emsmdbp_object *synccontext_object)
{
	void					*mem_ctx;
	struct idset				*new_idset, *old_idset;
	struct oxcfxics_sync_data		*sync_data;
	struct emsmdbp_context			*emsmdbp_ctx;
	struct emsmdbp_object_synccontext	*synccontext;

	emsmdbp_ctx = synccontext_object->emsmdbp_ctx;
	synccontext = synccontext_object->object.synccontext;
	ndr_push_uint32(ndr, NDR_SCALARS, PR_INCR_SYNC_STATE_BEGIN);

	mem_ctx = talloc_zero(NULL, void);

	sync_data = talloc_zero(mem_ctx, struct oxcfxics_sync_data);
	openchangedb_get_MailboxReplica(emsmdbp_ctx->oc_ctx, emsmdbp_ctx->username, NULL, &sync_data->replica_guid);
	sync_data->prop_index.eid = 0;
	sync_data->prop_index.last_modification_time = 1;
	sync_data->properties.cValues = 2;
	sync_data->properties.aulPropTag = talloc_array(sync_data, enum MAPITAGS, 2);
	sync_data->properties.aulPropTag[1] = PR_LAST_MODIFICATION_TIME;
	sync_data->ndr = ndr;
	sync_data->cutmarks_ndr = ndr_push_init_ctx(sync_data);
	ndr_set_flags(&sync_data->cutmarks_ndr->flags, LIBNDR_FLAG_NOALIGN);
	sync_data->cutmarks_ndr->offset = 0;
	sync_data->cnset_seen = RAWIDSET_make(sync_data, true);
	sync_data->eid_set = RAWIDSET_make(sync_data, false);

	if (synccontext->request.contents_mode) {
		sync_data->properties.aulPropTag[0] = PR_MID;

		if (synccontext->request.normal) {
			sync_data->table_type = EMSMDBP_TABLE_MESSAGE_TYPE;
			oxcfxics_fill_transfer_state_arrays(mem_ctx, emsmdbp_ctx, synccontext, sync_data, synccontext_object->parent_object);
		}

		if (synccontext->request.fai) {
			sync_data->table_type = EMSMDBP_TABLE_FAI_TYPE;
			oxcfxics_fill_transfer_state_arrays(mem_ctx, emsmdbp_ctx, synccontext, sync_data, synccontext_object->parent_object);
		}
	}
	else {
		sync_data->properties.aulPropTag[0] = PR_FID;
		sync_data->table_type = EMSMDBP_TABLE_FOLDER_TYPE;

		oxcfxics_fill_transfer_state_arrays(mem_ctx, emsmdbp_ctx, synccontext, sync_data, synccontext_object->parent_object);
	}

	/* for some reason, Exchange returns the same range for PidTagCnsetSeen, PidTagCnsetSeenFAI and PidTagCnsetRead */

	new_idset = RAWIDSET_convert_to_idset(NULL, sync_data->cnset_seen);
	old_idset = synccontext->cnset_seen;
	synccontext->cnset_seen = IDSET_merge_idsets(synccontext, old_idset, new_idset);
	talloc_free(old_idset);
	talloc_free(new_idset);

	ndr_push_uint32(ndr, NDR_SCALARS, PidTagCnsetSeen);
	ndr_push_idset(ndr, synccontext->cnset_seen);
	if (synccontext->request.contents_mode && synccontext->request.fai) {
		ndr_push_uint32(ndr, NDR_SCALARS, PidTagCnsetSeenFAI);
		ndr_push_idset(ndr, synccontext->cnset_seen);
	}

	new_idset = RAWIDSET_convert_to_idset(NULL, sync_data->eid_set);
	old_idset = synccontext->idset_given;
	synccontext->idset_given = IDSET_merge_idsets(synccontext, old_idset, new_idset);
	talloc_free(old_idset);
	talloc_free(new_idset);

	ndr_push_uint32(ndr, NDR_SCALARS, PidTagIdsetGiven);
	ndr_push_idset(ndr, synccontext->idset_given);

	if (synccontext->request.contents_mode && synccontext->request.read_state) {
		ndr_push_uint32(ndr, NDR_SCALARS, PidTagCnsetRead);
		ndr_push_idset(ndr, synccontext->cnset_seen);
	}

	talloc_free(mem_ctx);

	ndr_push_uint32(ndr, NDR_SCALARS, PR_INCR_SYNC_STATE_END);
}

/**
   \details EcDoRpc EcDoRpc_RopSyncGetTransferState (0x82) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SyncGetTransferState EcDoRpc_MAPI_REQ structure
   \param mapi_repl pointer to the SyncGetTransferState EcDoRpc_MAPI_REPL structure
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSyncGetTransferState(TALLOC_CTX *mem_ctx,
							 struct emsmdbp_context *emsmdbp_ctx,
							 struct EcDoRpc_MAPI_REQ *mapi_req,
							 struct EcDoRpc_MAPI_REPL *mapi_repl,
							 uint32_t *handles, uint16_t *size)
{
	uint32_t				synccontext_handle_id;
	struct mapi_handles			*synccontext_handle, *ftcontext_handle;
	struct emsmdbp_object			*synccontext_object, *ftcontext_object;
	struct emsmdbp_object_ftcontext		*ftcontext;
	enum MAPISTATUS				retval;
	void					*data = NULL;
	struct ndr_push				*ndr;

	DEBUG(4, ("exchange_emsmdb: [OXCFXICS] RopSyncGetTransferState (0x82)\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->error_code = MAPI_E_SUCCESS;
	mapi_repl->handle_idx = mapi_req->u.mapi_SyncGetTransferState.handle_idx;

	synccontext_handle_id = handles[mapi_req->handle_idx];
	retval = mapi_handles_search(emsmdbp_ctx->handles_ctx, synccontext_handle_id, &synccontext_handle);
	if (retval) {
		DEBUG(5, ("  handle (%x) not found: %x\n", synccontext_handle_id, mapi_req->handle_idx));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;
		goto end;
	}

	mapi_handles_get_private_data(synccontext_handle, &data);
	synccontext_object = (struct emsmdbp_object *)data;
	if (!synccontext_object || synccontext_object->type != EMSMDBP_OBJECT_SYNCCONTEXT) {
		DEBUG(5, ("  object not found or not a synccontext\n"));
		mapi_repl->error_code = MAPI_E_INVALID_OBJECT;	
		goto end;
	}

	ndr = ndr_push_init_ctx(NULL);
	ndr_set_flags(&ndr->flags, LIBNDR_FLAG_NOALIGN);
	ndr->offset = 0;

	oxcfxics_ndr_push_transfer_state(ndr, synccontext_object);

	retval = mapi_handles_add(emsmdbp_ctx->handles_ctx, synccontext_handle_id, &ftcontext_handle);
	ftcontext_object = emsmdbp_object_ftcontext_init((TALLOC_CTX *)ftcontext_handle, emsmdbp_ctx, synccontext_object);
	mapi_handles_set_private_data(ftcontext_handle, ftcontext_object);
	handles[mapi_repl->handle_idx] = ftcontext_handle->handle;

	ftcontext = ftcontext_object->object.ftcontext;
	(void) talloc_reference(ftcontext, ndr->data);
	ftcontext->stream.buffer.data = ndr->data;
	ftcontext->stream.buffer.length = ndr->offset;

	talloc_free(ndr);

	/* cutmarks */
	ndr = ndr_push_init_ctx(ftcontext);
	ndr_set_flags(&ndr->flags, LIBNDR_FLAG_NOALIGN);
	ndr->offset = 0;
	ndr_push_uint32(ndr, NDR_SCALARS, 0xffffffff);

	ftcontext->cutmarks = (uint32_t *) ndr->data;
	(void) talloc_reference(ftcontext, ndr->data);
	talloc_free(ndr);

end:
	*size += libmapiserver_RopSyncGetTransferState_size(mapi_repl);

	return MAPI_E_SUCCESS;
}

/**
   \details EcDoRpc SetLocalReplicaMidsetDeleted (0x93) Rop.

   \param mem_ctx pointer to the memory context
   \param emsmdbp_ctx pointer to the emsmdb provider context
   \param mapi_req pointer to the SetLocalReplicaMidsetDeleted EcDoRpc_MAPI_REQ
   \param mapi_repl pointer to the SetLocalReplicaMidsetDeleted EcDoRpc_MAPI_REPL
   \param handles pointer to the MAPI handles array
   \param size pointer to the mapi_response size to update

   \return MAPI_E_SUCCESS on success, otherwise MAPI error
 */
_PUBLIC_ enum MAPISTATUS EcDoRpc_RopSetLocalReplicaMidsetDeleted(TALLOC_CTX *mem_ctx,
								 struct emsmdbp_context *emsmdbp_ctx,
								 struct EcDoRpc_MAPI_REQ *mapi_req,
								 struct EcDoRpc_MAPI_REPL *mapi_repl,
								 uint32_t *handles, uint16_t *size)
{
	DEBUG(4, ("exchange_emsmdb: [OXCSTOR] SetLocalReplicaMidsetDeleted (0x93) - stub\n"));

	/* Sanity checks */
	OPENCHANGE_RETVAL_IF(!emsmdbp_ctx, MAPI_E_NOT_INITIALIZED, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_req, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!mapi_repl, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!handles, MAPI_E_INVALID_PARAMETER, NULL);
	OPENCHANGE_RETVAL_IF(!size, MAPI_E_INVALID_PARAMETER, NULL);

	mapi_repl->opnum = mapi_req->opnum;
	mapi_repl->handle_idx = mapi_req->handle_idx;
	mapi_repl->error_code = MAPI_E_SUCCESS;

	/* TODO effective work here */

	*size += libmapiserver_RopSetLocalReplicaMidsetDeleted_size(mapi_repl);

	handles[mapi_repl->handle_idx] = handles[mapi_req->handle_idx];

	return MAPI_E_SUCCESS;
}
