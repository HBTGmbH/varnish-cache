/*-
 * Copyright (c) 2024 Varnish Software AS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Accept header VMOD for cache normalization.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <cache/cache.h>
#include <vsb.h>

#include "vcc_acceptnorm_if.h"

#define MAX_MEDIA_TYPES 64

struct media_type {
	char		*type;		/* e.g., "text/html" */
	double		quality;	/* 0.0 to 1.0, default 1.0 */
};

struct acceptnorm_state {
	unsigned		magic;
#define ACCEPTNORM_STATE_MAGIC	0x41434350  /* "ACCP" */
	struct media_type	types[MAX_MEDIA_TYPES];
	int			count;
};

static void
acceptnorm_state_free(VRT_CTX, void *p)
{
	struct acceptnorm_state *state;
	int i;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CAST_OBJ_NOTNULL(state, p, ACCEPTNORM_STATE_MAGIC);

	for (i = 0; i < state->count; i++) {
		free(state->types[i].type);
	}
	FREE_OBJ(state);
}

static const struct vmod_priv_methods acceptnorm_state_methods[1] = {{
	.magic = VMOD_PRIV_METHODS_MAGIC,
	.type = "vmod_acceptnorm_state",
	.fini = acceptnorm_state_free
}};

static struct acceptnorm_state *
acceptnorm_state_get(struct vmod_priv *priv)
{
	struct acceptnorm_state *state;

	if (priv->priv == NULL) {
		ALLOC_OBJ(state, ACCEPTNORM_STATE_MAGIC);
		AN(state);
		state->count = 0;
		priv->priv = state;
		priv->methods = acceptnorm_state_methods;
	} else {
		CAST_OBJ_NOTNULL(state, priv->priv, ACCEPTNORM_STATE_MAGIC);
	}

	return (state);
}

static void
acceptnorm_state_clear(struct acceptnorm_state *state)
{
	int i;

	for (i = 0; i < state->count; i++) {
		free(state->types[i].type);
		state->types[i].type = NULL;
	}
	state->count = 0;
}

/* Skip whitespace */
static const char *
skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return (p);
}

/* Parse a single media type with optional quality */
static const char *
parse_media_type(const char *p, char **type_out, double *quality_out)
{
	const char *start, *end;
	char *type;
	double quality = 1.0;

	p = skip_ws(p);
	if (*p == '\0')
		return (NULL);

	/* Find the media type (up to ; or , or end) */
	start = p;
	while (*p && *p != ';' && *p != ',')
		p++;

	/* Trim trailing whitespace from type */
	end = p;
	while (end > start && isspace((unsigned char)*(end - 1)))
		end--;

	if (end <= start)
		return (NULL);

	type = strndup(start, end - start);
	AN(type);

	/* Convert to lowercase for consistent comparison */
	for (char *t = type; *t; t++)
		*t = tolower((unsigned char)*t);

	/* Parse parameters (looking for q=) */
	while (*p == ';') {
		const char *param_start;
		p = skip_ws(p + 1);
		param_start = p;

		/* Find parameter name */
		while (*p && *p != '=' && *p != ';' && *p != ',')
			p++;

		if (*p == '=' && (p - param_start == 1) &&
		    tolower((unsigned char)*param_start) == 'q') {
			/* Found q= parameter */
			p++;
			p = skip_ws(p);
			quality = strtod(p, (char **)&p);
			if (quality < 0.0)
				quality = 0.0;
			if (quality > 1.0)
				quality = 1.0;
		} else {
			/* Skip other parameters */
			if (*p == '=') {
				p++;
				while (*p && *p != ';' && *p != ',')
					p++;
			}
		}
	}

	/* Skip whitespace and comma if present */
	p = skip_ws(p);
	if (*p == ',')
		p++;

	*type_out = type;
	*quality_out = quality;
	return (p);
}

/* Parse Accept header into state */
static void
parse_accept_header(struct acceptnorm_state *state, const char *header)
{
	const char *p = header;
	char *type;
	double quality;

	acceptnorm_state_clear(state);

	if (header == NULL || *header == '\0')
		return;

	while (p && *p && state->count < MAX_MEDIA_TYPES) {
		p = parse_media_type(p, &type, &quality);
		if (p == NULL)
			break;

		state->types[state->count].type = type;
		state->types[state->count].quality = quality;
		state->count++;
	}
}

/* Comparison function for qsort: sort by quality desc, then alpha asc */
static int
media_type_cmp(const void *a, const void *b)
{
	const struct media_type *ma = a;
	const struct media_type *mb = b;

	/* Sort by quality descending */
	if (ma->quality > mb->quality)
		return (-1);
	if (ma->quality < mb->quality)
		return (1);

	/* Same quality: sort alphabetically ascending */
	return (strcmp(ma->type, mb->type));
}

/* Sort media types by quality (desc) then alphabetically (asc) */
static void
sort_media_types(struct acceptnorm_state *state)
{
	if (state->count > 1) {
		qsort(state->types, state->count, sizeof(struct media_type),
		    media_type_cmp);
	}
}

/* Check if two media types match (considering wildcards) */
static int
media_type_match(const char *pattern, const char *type)
{
	const char *p_slash, *t_slash;
	size_t p_type_len, t_type_len;

	if (strcmp(pattern, "*/*") == 0)
		return (1);

	p_slash = strchr(pattern, '/');
	t_slash = strchr(type, '/');

	if (p_slash == NULL || t_slash == NULL)
		return (strcmp(pattern, type) == 0);

	p_type_len = p_slash - pattern;
	t_type_len = t_slash - type;

	/* Check for "type / *" wildcard */
	if (strcmp(p_slash + 1, "*") == 0) {
		return (p_type_len == t_type_len &&
		    strncmp(pattern, type, p_type_len) == 0);
	}

	/* Exact match */
	return (strcmp(pattern, type) == 0);
}

/* Get quality for a media type from parsed state */
static double
get_quality_for_type(struct acceptnorm_state *state, const char *type)
{
	int i;
	double wildcard_quality = -1.0;
	double type_wildcard_quality = -1.0;
	char type_prefix[256];
	const char *slash;

	/* Build type prefix for "type / *" matching */
	slash = strchr(type, '/');
	if (slash) {
		size_t len = slash - type;
		if (len < sizeof(type_prefix) - 2) {
			strncpy(type_prefix, type, len);
			type_prefix[len] = '/';
			type_prefix[len + 1] = '*';
			type_prefix[len + 2] = '\0';
		} else {
			type_prefix[0] = '\0';
		}
	} else {
		type_prefix[0] = '\0';
	}

	for (i = 0; i < state->count; i++) {
		/* Exact match takes priority */
		if (strcmp(state->types[i].type, type) == 0)
			return (state->types[i].quality);

		/* Track wildcard matches */
		if (strcmp(state->types[i].type, "*/*") == 0)
			wildcard_quality = state->types[i].quality;
		else if (type_prefix[0] &&
		    strcmp(state->types[i].type, type_prefix) == 0)
			type_wildcard_quality = state->types[i].quality;
	}

	/* Return most specific wildcard match */
	if (type_wildcard_quality >= 0.0)
		return (type_wildcard_quality);
	if (wildcard_quality >= 0.0)
		return (wildcard_quality);

	return (0.0);
}

/* Build Accept header string from state */
static const char *
build_accept_string(VRT_CTX, struct acceptnorm_state *state)
{
	struct vsb *vsb;
	const char *result;
	int i;

	if (state->count == 0)
		return ("");

	vsb = VSB_new_auto();
	AN(vsb);

	for (i = 0; i < state->count; i++) {
		if (i > 0)
			VSB_cat(vsb, ", ");
		VSB_cat(vsb, state->types[i].type);
		if (state->types[i].quality < 1.0) {
			VSB_printf(vsb, ";q=%.1f", state->types[i].quality);
		}
	}

	AZ(VSB_finish(vsb));
	result = WS_Copy(ctx->ws, VSB_data(vsb), -1);
	VSB_destroy(&vsb);

	return (result ? result : "");
}

/* Parse preferred types list */
static int
parse_preferred_types(const char *preferred, char **types, int max_types)
{
	const char *p = preferred;
	int count = 0;

	if (preferred == NULL || *preferred == '\0')
		return (0);

	while (*p && count < max_types) {
		const char *start, *end;

		p = skip_ws(p);
		if (*p == '\0')
			break;

		start = p;
		while (*p && *p != ',')
			p++;

		end = p;
		while (end > start && isspace((unsigned char)*(end - 1)))
			end--;

		if (end > start) {
			types[count] = strndup(start, end - start);
			AN(types[count]);
			/* Convert to lowercase */
			for (char *t = types[count]; *t; t++)
				*t = tolower((unsigned char)*t);
			count++;
		}

		if (*p == ',')
			p++;
	}

	return (count);
}

static void
free_preferred_types(char **types, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		free(types[i]);
	}
}

/*--------------------------------------------------------------------
 * VMOD functions
 */

VCL_STRING v_matchproto_(td_acceptnorm_canonicalize)
vmod_canonicalize(VRT_CTX, struct vmod_priv *priv, VCL_STRING accept_header)
{
	struct acceptnorm_state *state;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	state = acceptnorm_state_get(priv);

	if (accept_header == NULL || *accept_header == '\0')
		return ("");

	parse_accept_header(state, accept_header);
	sort_media_types(state);

	return (build_accept_string(ctx, state));
}

VCL_STRING v_matchproto_(td_acceptnorm_filter)
vmod_filter(VRT_CTX, struct vmod_priv *priv, VCL_STRING accept_header,
    VCL_STRING preferred)
{
	struct acceptnorm_state *state;
	struct acceptnorm_state filtered;
	char *pref_types[MAX_MEDIA_TYPES];
	int pref_count, i, j;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	state = acceptnorm_state_get(priv);

	if (preferred == NULL || *preferred == '\0')
		return (vmod_canonicalize(ctx, priv, accept_header));

	if (accept_header == NULL || *accept_header == '\0') {
		/* No Accept header: return first preferred type */
		pref_count = parse_preferred_types(preferred, pref_types,
		    MAX_MEDIA_TYPES);
		if (pref_count > 0) {
			result = WS_Copy(ctx->ws, pref_types[0], -1);
			free_preferred_types(pref_types, pref_count);
			return (result ? result : "");
		}
		return ("");
	}

	parse_accept_header(state, accept_header);
	pref_count = parse_preferred_types(preferred, pref_types,
	    MAX_MEDIA_TYPES);

	/* Build filtered list: only preferred types that client accepts */
	INIT_OBJ(&filtered, ACCEPTNORM_STATE_MAGIC);
	filtered.count = 0;

	for (i = 0; i < pref_count && filtered.count < MAX_MEDIA_TYPES; i++) {
		double quality = 0.0;

		/* Check if client accepts this preferred type */
		for (j = 0; j < state->count; j++) {
			if (media_type_match(state->types[j].type,
			    pref_types[i])) {
				if (state->types[j].quality > quality)
					quality = state->types[j].quality;
			}
		}

		if (quality > 0.0) {
			filtered.types[filtered.count].type =
			    strdup(pref_types[i]);
			AN(filtered.types[filtered.count].type);
			filtered.types[filtered.count].quality = quality;
			filtered.count++;
		}
	}

	/* If no matches, return first preferred as fallback */
	if (filtered.count == 0 && pref_count > 0) {
		filtered.types[0].type = strdup(pref_types[0]);
		AN(filtered.types[0].type);
		filtered.types[0].quality = 1.0;
		filtered.count = 1;
	}

	free_preferred_types(pref_types, pref_count);

	sort_media_types(&filtered);
	result = build_accept_string(ctx, &filtered);

	/* Clean up filtered state */
	for (i = 0; i < filtered.count; i++)
		free(filtered.types[i].type);

	return (result);
}

VCL_STRING v_matchproto_(td_acceptnorm_best_match)
vmod_best_match(VRT_CTX, struct vmod_priv *priv, VCL_STRING accept_header,
    VCL_STRING preferred)
{
	struct acceptnorm_state *state;
	char *pref_types[MAX_MEDIA_TYPES];
	int pref_count, i, j;
	const char *best_type = NULL;
	double best_quality = -1.0;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	state = acceptnorm_state_get(priv);

	pref_count = parse_preferred_types(preferred, pref_types,
	    MAX_MEDIA_TYPES);

	if (pref_count == 0)
		return ("");

	if (accept_header == NULL || *accept_header == '\0') {
		/* No Accept header: return first preferred */
		result = WS_Copy(ctx->ws, pref_types[0], -1);
		free_preferred_types(pref_types, pref_count);
		return (result ? result : "");
	}

	parse_accept_header(state, accept_header);

	/* Find best matching preferred type */
	for (i = 0; i < pref_count; i++) {
		double quality = 0.0;

		for (j = 0; j < state->count; j++) {
			if (media_type_match(state->types[j].type,
			    pref_types[i])) {
				if (state->types[j].quality > quality)
					quality = state->types[j].quality;
			}
		}

		/* Prefer higher quality, or earlier in preferred list for ties */
		if (quality > best_quality) {
			best_quality = quality;
			best_type = pref_types[i];
		}
	}

	/* Fallback to first preferred if no match */
	if (best_type == NULL)
		best_type = pref_types[0];

	result = WS_Copy(ctx->ws, best_type, -1);
	free_preferred_types(pref_types, pref_count);

	return (result ? result : "");
}

VCL_STRING v_matchproto_(td_acceptnorm_prefer)
vmod_prefer(VRT_CTX, struct vmod_priv *priv, VCL_STRING accept_header,
    VCL_STRING preferred)
{
	struct acceptnorm_state *state;
	char *pref_types[MAX_MEDIA_TYPES];
	int pref_count, i, j;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	state = acceptnorm_state_get(priv);

	if (accept_header == NULL || *accept_header == '\0')
		return ("");

	pref_count = parse_preferred_types(preferred, pref_types,
	    MAX_MEDIA_TYPES);

	if (pref_count == 0)
		return (accept_header);

	parse_accept_header(state, accept_header);

	/* Find first preferred type that client accepts (q > 0) */
	for (i = 0; i < pref_count; i++) {
		for (j = 0; j < state->count; j++) {
			if (media_type_match(state->types[j].type,
			    pref_types[i]) &&
			    state->types[j].quality > 0.0) {
				/* Found a match - return this preferred type */
				result = WS_Copy(ctx->ws, pref_types[i], -1);
				free_preferred_types(pref_types, pref_count);
				return (result ? result : "");
			}
		}
	}

	/* No preferred type matches - return original Accept header as-is */
	free_preferred_types(pref_types, pref_count);
	return (accept_header);
}

VCL_REAL v_matchproto_(td_acceptnorm_quality)
vmod_quality(VRT_CTX, struct vmod_priv *priv, VCL_STRING accept_header,
    VCL_STRING media_type)
{
	struct acceptnorm_state *state;
	char *type_lower;
	double quality;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	(void)priv;

	if (accept_header == NULL || *accept_header == '\0')
		return (0.0);

	if (media_type == NULL || *media_type == '\0')
		return (0.0);

	state = acceptnorm_state_get(priv);
	parse_accept_header(state, accept_header);

	/* Convert media_type to lowercase for comparison */
	type_lower = strdup(media_type);
	AN(type_lower);
	for (char *t = type_lower; *t; t++)
		*t = tolower((unsigned char)*t);

	quality = get_quality_for_type(state, type_lower);
	free(type_lower);

	return (quality);
}

VCL_BOOL v_matchproto_(td_acceptnorm_accepts)
vmod_accepts(VRT_CTX, struct vmod_priv *priv, VCL_STRING accept_header,
    VCL_STRING media_type)
{
	return (vmod_quality(ctx, priv, accept_header, media_type) > 0.0);
}
