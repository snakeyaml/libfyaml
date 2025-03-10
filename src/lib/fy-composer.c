/*
 * fy-composer.c - Composer support
 *
 * Copyright (c) 2021 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>

#include <libfyaml.h>

#include "fy-parse.h"
#include "fy-doc.h"

#include "fy-utils.h"

#undef DBG
// #define DBG fyp_notice
#define DBG fyp_scan_debug

struct fy_composer *
fy_composer_create(struct fy_composer_cfg *cfg)
{
	struct fy_composer *fyc;
	struct fy_path *fypp;

	/* verify configuration and mandatory ops */
	if (!cfg || !cfg->ops ||
	    !cfg->ops->process_event)
		return NULL;

	fyc = malloc(sizeof(*fyc));
	if (!fyc)
		return NULL;
	memset(fyc, 0, sizeof(*fyc));
	fyc->cfg = *cfg;

	fy_path_list_init(&fyc->paths);
	fypp = fy_path_create();
	if (!fypp)
		goto err_no_path;
	fy_path_list_add_tail(&fyc->paths, fypp);

	return fyc;

err_no_path:
	free(fyc);
	return NULL;

}

void fy_composer_destroy(struct fy_composer *fyc)
{
	struct fy_path *fypp;

	if (!fyc)
		return;

	fy_diag_unref(fyc->cfg.diag);
	while ((fypp = fy_path_list_pop(&fyc->paths)) != NULL)
		fy_path_destroy(fypp);
	free(fyc);
}

static enum fy_composer_return
fy_composer_process_event_private(struct fy_composer *fyc, struct fy_parser *fyp,
				  struct fy_event *fye, struct fy_path *fypp)
{
	const struct fy_composer_ops *ops;
	struct fy_eventp *fyep;
	struct fy_path_component *fypc, *fypc_last;
	struct fy_path *fyppt;
	struct fy_document *fyd;
	bool is_collection, is_map, is_start, is_end;
	char tbuf[80] __attribute__((__unused__));
	int rc = 0;
	enum fy_composer_return ret;
	bool stop_req = false;

	assert(fyc);
	assert(fyp);
	assert(fye);
	assert(fypp);

	fyep = container_of(fye, struct fy_eventp, e);

	ops = fyc->cfg.ops;
	assert(ops);

	rc = 0;

	switch (fye->type) {
	case FYET_MAPPING_START:
		is_collection = true;
		is_start = true;
		is_end = false;
		is_map = true;
		break;

	case FYET_MAPPING_END:
		is_collection = true;
		is_start = false;
		is_end = true;
		is_map = true;
		break;

	case FYET_SEQUENCE_START:
		is_collection = true;
		is_start = true;
		is_end = false;
		is_map = false;
		break;

	case FYET_SEQUENCE_END:
		is_collection = true;
		is_start = false;
		is_end = true;
		is_map = false;
		break;

	case FYET_SCALAR:
		is_collection = false;
		is_start = true;
		is_end = true;
		is_map = false;
		break;

	case FYET_ALIAS:
		is_collection = false;
		is_start = true;
		is_end = true;
		is_map = false;
		break;

	case FYET_STREAM_START:
	case FYET_STREAM_END:
	case FYET_DOCUMENT_START:
	case FYET_DOCUMENT_END:
		// fprintf(stderr, "%s:%d process_event\n", __FILE__, __LINE__);
		return ops->process_event(fyc, fypp, fyp, fye);

	default:
		// DBG(fyp, "ignoring\n");
		return FYCR_OK_CONTINUE;
	}

	fypc_last = fy_path_component_list_tail(&fypp->components);

	// DBG(fyp, "%s: start - %s\n", fy_path_get_text0(fypp), fy_token_dump_format(fyt, tbuf, sizeof(tbuf)));

	if (fy_path_component_is_mapping(fypc_last) && fypc_last->map.accumulating_complex_key) {

		// DBG(fyp, "accumulating for complex key\n");
		// fprintf(stderr, "accumulating for complex key %s\n",
		//		fy_token_dump_format(fy_event_get_token(fye), tbuf, sizeof(tbuf)));

		/* get the next one */
		fyppt = fy_path_next(&fyc->paths, fypp);
		assert(fyppt);
		assert(fyppt != fypp);
		assert(fyppt->parent == fypp);

		/* and pass along */
		ret = fy_composer_process_event_private(fyc, fyp, fye, fyppt);
		if (!fy_composer_return_is_ok(ret)) {
			/* XXX TODO handle skip */
			return ret;
		}
		if (!stop_req)
			stop_req = ret == FYCR_OK_STOP;

		rc = fy_document_builder_process_event(fypp->fydb, fyp, fyep);
		if (rc == 0) {
			// DBG(fyp, "accumulating still\n");
			// fprintf(stderr, "accumulating still %s\n",
			//		fy_token_dump_format(fy_event_get_token(fye), tbuf, sizeof(tbuf)));
			return FYCR_OK_CONTINUE;
		}
		fyc_error_check(fyc, rc > 0, err_out,
				"fy_document_builder_process_event() failed\n");

		// fprintf(stderr, "accumulating complete %s\n",
		//		fy_token_dump_format(fy_event_get_token(fye), tbuf, sizeof(tbuf)));

		// DBG(fyp, "accumulation complete\n");

		/* get the document */
		fyd = fy_document_builder_take_document(fypp->fydb);
		fyc_error_check(fyc, fyd, err_out,
				"fy_document_builder_take_document() failed\n");

		fypc_last->map.is_complex_key = true;
		fypc_last->map.accumulating_complex_key = false;
		fypc_last->map.complex_key = fyd;
		fypc_last->map.has_key = true;
		fypc_last->map.await_key = false;
		fypc_last->map.complex_key_complete = true;
		fypc_last->map.root = false;

		fyppt = fy_path_list_pop_tail(&fyc->paths);
		assert(fyppt);

		fy_path_destroy(fyppt);

		// DBG(fyp, "%s: %s complex KEY\n", __func__, fy_path_get_text0(fypp));

		fyc_error_check(fyc, rc >= 0, err_out,
				"fy_path_component_build_text() failed\n");

		return !stop_req ? FYCR_OK_CONTINUE : FYCR_OK_STOP;
	}

	/* start of something on a mapping */
	if (is_start && fy_path_component_is_mapping(fypc_last) && fypc_last->map.await_key && is_collection) {

		/* non scalar key case */
		// DBG(fyp, "Non scalar key - using document builder\n");
		if (!fypp->fydb) {
			fypp->fydb = fy_document_builder_create(&fyp->cfg);
			fyc_error_check(fyc, fypp->fydb, err_out,
					"fy_document_builder_create() failed\n");
		}

		/* start with this document state */
		rc = fy_document_builder_set_in_document(fypp->fydb, fy_parser_get_document_state(fyp), true);
		fyc_error_check(fyc, !rc, err_out,
				"fy_document_builder_set_in_document() failed\n");

		// fprintf(stderr, "initial complex key %s\n",
		//		fy_token_dump_format(fy_event_get_token(fye), tbuf, sizeof(tbuf)));

		/* and pass the current event; must return 0 since we know it's a collection start */
		rc = fy_document_builder_process_event(fypp->fydb, fyp, fyep);
		fyc_error_check(fyc, !rc, err_out,
				"fy_document_builder_process_event() failed\n");

		fypc_last->map.is_complex_key = true;
		fypc_last->map.accumulating_complex_key = true;
		fypc_last->map.complex_key = NULL;
		fypc_last->map.complex_key_complete = false;

		/* create new path */
		fyppt = fy_path_create();
		fyc_error_check(fyc, fyppt, err_out,
				"fy_path_create() failed\n");

		/* append it to the end */
		fyppt->parent = fypp;
		fy_path_list_add_tail(&fyc->paths, fyppt);

		/* and pass along */
		ret = fy_composer_process_event_private(fyc, fyp, fye, fyppt);
		if (!fy_composer_return_is_ok(ret)) {
			/* XXX TODO handle skip */
			return ret;
		}
		if (!stop_req)
			stop_req = ret == FYCR_OK_STOP;

		return !stop_req ? FYCR_OK_CONTINUE : FYCR_OK_STOP;
	}

	if (is_start && fy_path_component_is_sequence(fypc_last)) {	/* start in a sequence */

		if (fypc_last->seq.idx < 0)
			fypc_last->seq.idx = 0;
		else
			fypc_last->seq.idx++;
	}

	if (is_collection && is_start) {

		/* collection start */
		if (is_map) {
			fypc = fy_path_component_create_mapping(fypp);
			fyc_error_check(fyc, fypc, err_out,
					"fy_path_component_create_mapping() failed\n");
		} else {
			fypc = fy_path_component_create_sequence(fypp);
			fyc_error_check(fyc, fypc, err_out,
					"fy_path_component_create_sequence() failed\n");
		}

		/* append to the tail */
		fy_path_component_list_add_tail(&fypp->components, fypc);

	} else if (is_collection && is_end) {

		/* collection end */
		assert(fypc_last);
		fy_path_component_clear_state(fypc_last);

	} else if (!is_collection && fy_path_component_is_mapping(fypc_last) && fypc_last->map.await_key) {

		// DBG(fyp, "scalar key: %s\n", fy_token_dump_format(fyt, tbuf, sizeof(tbuf)));
		fypc_last->map.is_complex_key = false;
		fypc_last->map.scalar.tag = fy_token_ref(fy_event_get_tag_token(fye));
		fypc_last->map.scalar.key = fy_token_ref(fy_event_get_token(fye));
		fypc_last->map.has_key = true;
		fypc_last->map.root = false;

	}

	/* process the event */
	ret = ops->process_event(fyc, fypp, fyp, fye);
	if (!fy_composer_return_is_ok(ret)) {
		/* XXX TODO handle skip */
		return ret;
	}
	if (!stop_req)
		stop_req = ret == FYCR_OK_STOP;

	if (is_collection && is_end) {
		/* for the end of a collection, pop the last component */
		fypc = fy_path_component_list_pop_tail(&fypp->components);
		assert(fypc);

		assert(fypc == fypc_last);

		fy_path_component_recycle(fypp, fypc);

		/* and get the new last */
		fypc_last = fy_path_component_list_tail(&fypp->components);
	}

	/* at the end of something */
	if (is_end && fy_path_component_is_mapping(fypc_last)) {

		if (!fypc_last->map.await_key) {

			fy_path_component_clear_state(fypc_last);

			// DBG(fyp, "%s: set await_key %p\n", fy_path_get_text0(fypp), fypc_last);
			fypc_last->map.await_key = true;

		} else {
			fypc_last->map.await_key = false;
		}
	}

	// DBG(fyp, "%s: exit\n", fy_path_get_text0(fypp));
	return !stop_req ? FYCR_OK_CONTINUE : FYCR_OK_STOP;

err_out:
	return FYCR_ERROR;
}

enum fy_composer_return
fy_composer_process_event(struct fy_composer *fyc, struct fy_parser *fyp, struct fy_event *fye)
{
	struct fy_path *fypp;
	int rc;

	if (!fyc || !fyp || !fye)
		return -1;

	/* start at the head */
	fypp = fy_path_list_head(&fyc->paths);

	/* no top? something's very out of order */
	if (!fypp)
		return -1;

	rc = fy_composer_process_event_private(fyc, fyp, fye, fypp);

	return rc;
}

struct fy_composer_cfg *fy_composer_get_cfg(struct fy_composer *fyc)
{
	if (!fyc)
		return NULL;
	return &fyc->cfg;
}

void *fy_composer_get_cfg_userdata(struct fy_composer *fyc)
{
	if (!fyc)
		return NULL;
	return fyc->cfg.userdata;
}

struct fy_diag *fy_composer_get_diag(struct fy_composer *fyc)
{
	if (!fyc)
		return NULL;
	return fyc->cfg.diag;
}

enum fy_composer_return
fy_composer_parse(struct fy_composer *fyc, struct fy_parser *fyp)
{
	struct fy_document_iterator *fydi;
	struct fy_event *fye;
	struct fy_eventp *fyep;
	struct fy_document *fyd = NULL;
	enum fy_composer_return ret;

	if (!fyp || !fyc)
		return FYCR_ERROR;

	/* simple, without resolution */
	if (!(fyp->cfg.flags & FYPCF_RESOLVE_DOCUMENT)) {

		ret = FYCR_OK_STOP;
		while ((fyep = fy_parse_private(fyp)) != NULL) {
			ret = fy_composer_process_event(fyc, fyp, &fyep->e);
			fy_parse_eventp_recycle(fyp, fyep);
			if (ret != FYCR_OK_CONTINUE)
				break;
		}
		return ret;
	}

	fydi = fy_document_iterator_create();
	if (!fydi)
		goto err_out;

	/* stream start event generation and processing */
	fye = fy_document_iterator_stream_start(fydi);
	if (!fye)
		goto err_out;
	ret = fy_composer_process_event(fyc, fyp, fye);
	fy_document_iterator_event_free(fydi, fye);
	fye = NULL;
	if (ret != FYCR_OK_CONTINUE)
		goto out;

	/* convert to document and then process the generator event stream it */
	while ((fyd = fy_parse_load_document(fyp)) != NULL) {

		/* document start event generation and processing */
		fye = fy_document_iterator_document_start(fydi, fyd);
		if (!fye)
			goto err_out;
		ret = fy_composer_process_event(fyc, fyp, fye);
		fy_document_iterator_event_free(fydi, fye);
		fye = NULL;
		if (ret != FYCR_OK_CONTINUE)
			goto out;

		/* and now process the body */
		ret = FYCR_OK_CONTINUE;
		while ((fye = fy_document_iterator_body_next(fydi)) != NULL) {
			ret = fy_composer_process_event(fyc, fyp, fye);
			fy_document_iterator_event_free(fydi, fye);
			fye = NULL;
			if (ret != FYCR_OK_CONTINUE)
				goto out;
		}

		/* document end event generation and processing */
		fye = fy_document_iterator_document_end(fydi);
		if (!fye)
			goto err_out;
		ret = fy_composer_process_event(fyc, fyp, fye);
		fy_document_iterator_event_free(fydi, fye);
		fye = NULL;
		if (ret != FYCR_OK_CONTINUE)
			goto out;

		/* and destroy the document */
		fy_parse_document_destroy(fyp, fyd);
		fyd = NULL;
	}

	/* stream end event generation and processing */
	fye = fy_document_iterator_stream_end(fydi);
	if (!fye)
		goto err_out;
	ret = fy_composer_process_event(fyc, fyp, fye);
	fy_document_iterator_event_free(fydi, fye);
	fye = NULL;
	if (ret != FYCR_OK_CONTINUE)
		goto out;

out:
	/* NULLs are OK */
	fy_parse_document_destroy(fyp, fyd);
	fy_document_iterator_destroy(fydi);
	return ret;

err_out:
	ret = FYCR_ERROR;
	goto out;
}
