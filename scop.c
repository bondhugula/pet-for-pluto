/*
 * Copyright 2011      Leiden University. All rights reserved.
 * Copyright 2012-2014 Ecole Normale Superieure. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY LEIDEN UNIVERSITY ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LEIDEN UNIVERSITY OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as
 * representing official policies, either expressed or implied, of
 * Leiden University.
 */ 

#include <string.h>
#include <isl/constraint.h>
#include <isl/union_set.h>

#include "aff.h"
#include "expr.h"
#include "filter.h"
#include "loc.h"
#include "nest.h"
#include "scop.h"
#include "print.h"
#include "value_bounds.h"

/* pet_scop with extra information that is used during parsing and printing.
 *
 * In particular, we keep track of conditions under which we want
 * to skip the rest of the current loop iteration (skip[pet_skip_now])
 * and of conditions under which we want to skip subsequent
 * loop iterations (skip[pet_skip_later]).
 *
 * The conditions are represented as index expressions defined
 * over the outer loop iterators.  The index expression is either
 * a boolean affine expression or an access to a variable, which
 * is assumed to attain values zero and one.  The condition holds
 * if the variable has value one or if the affine expression
 * has value one (typically for only part of the domain).
 *
 * A missing condition (skip[type] == NULL) means that we don't want
 * to skip anything.
 *
 * Additionally, we keep track of the original input file
 * inside pet_transform_C_source.
 */
struct pet_scop_ext {
	struct pet_scop scop;

	isl_multi_pw_aff *skip[2];
	FILE *input;
};

/* Construct a pet_stmt with given domain, location and statement
 * number from a pet_expr.
 * The input domain is anonymous and is the same as the domains
 * of the access expressions inside "expr".
 * These domains are modified to include the name of the statement.
 * This name is given by "label" if it is non-NULL.
 * Otherwise, the name is constructed as S_<id>.
 */
struct pet_stmt *pet_stmt_from_pet_expr(__isl_take isl_set *domain,
	__isl_take pet_loc *loc, __isl_take isl_id *label, int id,
	__isl_take pet_expr *expr)
{
	struct pet_stmt *stmt;
	isl_ctx *ctx;
	isl_space *space;
	isl_map *sched;
	isl_multi_aff *ma;
	isl_multi_pw_aff *add_name;
	char name[50];

	if (!domain || !loc || !expr)
		goto error;

	ctx = pet_expr_get_ctx(expr);
	stmt = isl_calloc_type(ctx, struct pet_stmt);
	if (!stmt)
		goto error;

	if (!label) {
		snprintf(name, sizeof(name), "S_%d", id);
		label = isl_id_alloc(ctx, name, NULL);
	}
	domain = isl_set_set_tuple_id(domain, label);
	space = isl_set_get_space(domain);
	space = pet_nested_remove_from_space(space);
	sched = isl_map_universe(isl_space_from_domain(isl_space_copy(space)));
	ma = pet_prefix_projection(space, isl_space_dim(space, isl_dim_set));

	add_name = isl_multi_pw_aff_from_multi_aff(ma);
	expr = pet_expr_update_domain(expr, add_name);

	stmt->loc = loc;
	stmt->domain = domain;
	stmt->schedule = sched;
	stmt->body = expr;

	if (!stmt->domain || !stmt->schedule || !stmt->body)
		return pet_stmt_free(stmt);

	return stmt;
error:
	isl_set_free(domain);
	isl_id_free(label);
	pet_loc_free(loc);
	pet_expr_free(expr);
	return NULL;
}

void *pet_stmt_free(struct pet_stmt *stmt)
{
	int i;

	if (!stmt)
		return NULL;

	pet_loc_free(stmt->loc);
	isl_set_free(stmt->domain);
	isl_map_free(stmt->schedule);
	pet_expr_free(stmt->body);

	for (i = 0; i < stmt->n_arg; ++i)
		pet_expr_free(stmt->args[i]);
	free(stmt->args);

	free(stmt);
	return NULL;
}

/* Return the iteration space of "stmt".
 *
 * If the statement has arguments, then stmt->domain is a wrapped map
 * mapping the iteration domain to the values of the arguments
 * for which this statement is executed.
 * In this case, we need to extract the domain space of this wrapped map.
 */
__isl_give isl_space *pet_stmt_get_space(struct pet_stmt *stmt)
{
	isl_space *space;

	if (!stmt)
		return NULL;

	space = isl_set_get_space(stmt->domain);
	if (isl_space_is_wrapping(space))
		space = isl_space_domain(isl_space_unwrap(space));

	return space;
}

static void stmt_dump(struct pet_stmt *stmt, int indent)
{
	int i;

	if (!stmt)
		return;

	fprintf(stderr, "%*s%d\n", indent, "", pet_loc_get_line(stmt->loc));
	fprintf(stderr, "%*s", indent, "");
	isl_set_dump(stmt->domain);
	fprintf(stderr, "%*s", indent, "");
	isl_map_dump(stmt->schedule);
	pet_expr_dump_with_indent(stmt->body, indent);
	for (i = 0; i < stmt->n_arg; ++i)
		pet_expr_dump_with_indent(stmt->args[i], indent + 2);
}

void pet_stmt_dump(struct pet_stmt *stmt)
{
	stmt_dump(stmt, 0);
}

/* Allocate a new pet_type with the given "name" and "definition".
 */
struct pet_type *pet_type_alloc(isl_ctx *ctx, const char *name,
	const char *definition)
{
	struct pet_type *type;

	type = isl_alloc_type(ctx, struct pet_type);
	if (!type)
		return NULL;

	type->name = strdup(name);
	type->definition = strdup(definition);

	if (!type->name || !type->definition)
		return pet_type_free(type);

	return type;
}

/* Free "type" and return NULL.
 */
struct pet_type *pet_type_free(struct pet_type *type)
{
	if (!type)
		return NULL;

	free(type->name);
	free(type->definition);

	free(type);
	return NULL;
}

struct pet_array *pet_array_free(struct pet_array *array)
{
	if (!array)
		return NULL;

	isl_set_free(array->context);
	isl_set_free(array->extent);
	isl_set_free(array->value_bounds);
	free(array->element_type);

	free(array);
	return NULL;
}

void pet_array_dump(struct pet_array *array)
{
	if (!array)
		return;

	isl_set_dump(array->context);
	isl_set_dump(array->extent);
	isl_set_dump(array->value_bounds);
	fprintf(stderr, "%s%s%s\n", array->element_type,
		array->element_is_record ? " element-is-record" : "",
		array->live_out ? " live-out" : "");
}

/* Alloc a pet_scop structure, with extra room for information that
 * is only used during parsing.
 */
struct pet_scop *pet_scop_alloc(isl_ctx *ctx)
{
	return &isl_calloc_type(ctx, struct pet_scop_ext)->scop;
}

/* Construct a pet_scop in the given space and with room for n statements.
 *
 * The context is initialized as a universe set in "space".
 *
 * Since no information on the location is known at this point,
 * scop->loc is initialized with pet_loc_dummy.
 */
static struct pet_scop *scop_alloc(__isl_take isl_space *space, int n)
{
	isl_ctx *ctx;
	struct pet_scop *scop;

	if (!space)
		return NULL;

	ctx = isl_space_get_ctx(space);
	scop = pet_scop_alloc(ctx);
	if (!scop)
		return NULL;

	scop->context = isl_set_universe(isl_space_copy(space));
	scop->context_value = isl_set_universe(isl_space_params(space));
	scop->stmts = isl_calloc_array(ctx, struct pet_stmt *, n);
	if (!scop->context || !scop->stmts)
		return pet_scop_free(scop);

	scop->loc = &pet_loc_dummy;
	scop->n_stmt = n;

	return scop;
}

/* Construct a pet_scop in the given space containing 0 statements.
 */
struct pet_scop *pet_scop_empty(__isl_take isl_space *space)
{
	return scop_alloc(space, 0);
}

/* Return the constraints on the iteration domain in the access relation
 * "access".
 * If the corresponding access expression has arguments then the domain
 * of "access" is a wrapped relation with the iteration domain in the domain
 * and the arguments in the range.
 */
static __isl_give isl_set *access_domain(__isl_take isl_map *access)
{
	isl_set *domain;

	domain = isl_map_domain(access);
	if (isl_set_is_wrapping(domain))
		domain = isl_map_domain(isl_set_unwrap(domain));

	return domain;
}

/* Update "context" with the constraints imposed on the outer iteration
 * domain by "access".
 * "context" lives in an anonymous space, while the domain of "access"
 * refers to a particular statement.  This reference therefore needs to be
 * stripped off.
 */
static __isl_give isl_set *access_extract_context(__isl_keep isl_map *access,
	__isl_take isl_set *context)
{
	isl_set *domain;

	domain = access_domain(isl_map_copy(access));
	domain = isl_set_reset_tuple_id(domain);
	context = isl_set_intersect(context, domain);
	return context;
}

/* Update "context" with the constraints imposed on the outer iteration
 * domain by "expr".
 *
 * "context" lives in an anonymous space, while the domains of
 * the access relations in "expr" refer to a particular statement.
 * This reference therefore needs to be stripped off.
 *
 * If "expr" represents a conditional operator, then a parameter or outer
 * iterator value needs to be valid for the condition and
 * for at least one of the remaining two arguments.
 * If the condition is an affine expression, then we can be a bit more specific.
 * The value then has to be valid for the second argument for
 * non-zero accesses and valid for the third argument for zero accesses.
 */
static __isl_give isl_set *expr_extract_context(__isl_keep pet_expr *expr,
	__isl_take isl_set *context)
{
	int i;

	if (expr->type == pet_expr_op && expr->op == pet_op_cond) {
		int is_aff;
		isl_set *context1, *context2;

		is_aff = pet_expr_is_affine(expr->args[0]);
		if (is_aff < 0)
			goto error;

		context = expr_extract_context(expr->args[0], context);
		context1 = expr_extract_context(expr->args[1],
						isl_set_copy(context));
		context2 = expr_extract_context(expr->args[2], context);

		if (is_aff) {
			isl_map *access;
			isl_set *zero_set;

			access = isl_map_copy(expr->args[0]->acc.access);
			access = isl_map_fix_si(access, isl_dim_out, 0, 0);
			zero_set = access_domain(access);
			zero_set = isl_set_reset_tuple_id(zero_set);
			context1 = isl_set_subtract(context1,
						    isl_set_copy(zero_set));
			context2 = isl_set_intersect(context2, zero_set);
		}

		context = isl_set_union(context1, context2);
		context = isl_set_coalesce(context);

		return context;
	}

	for (i = 0; i < expr->n_arg; ++i)
		context = expr_extract_context(expr->args[i], context);

	if (expr->type == pet_expr_access)
		context = access_extract_context(expr->acc.access, context);

	return context;
error:
	isl_set_free(context);
	return NULL;
}

/* Update "context" with the constraints imposed on the outer iteration
 * domain by "stmt".
 *
 * If the statement is an assume statement with an affine expression,
 * then intersect "context" with that expression.
 * Otherwise, intersect "context" with the contexts of the expressions
 * inside "stmt".
 */
static __isl_give isl_set *stmt_extract_context(struct pet_stmt *stmt,
	__isl_take isl_set *context)
{
	int i;

	if (pet_stmt_is_assume(stmt) &&
	    pet_expr_is_affine(stmt->body->args[0])) {
		isl_multi_pw_aff *index;
		isl_pw_aff *pa;
		isl_set *cond;

		index = stmt->body->args[0]->acc.index;
		pa = isl_multi_pw_aff_get_pw_aff(index, 0);
		cond = isl_pw_aff_non_zero_set(pa);
		cond = isl_set_reset_tuple_id(cond);
		return isl_set_intersect(context, cond);
	}

	for (i = 0; i < stmt->n_arg; ++i)
		context = expr_extract_context(stmt->args[i], context);

	context = expr_extract_context(stmt->body, context);

	return context;
}

/* Construct a pet_scop in the given space that contains the given pet_stmt.
 */
struct pet_scop *pet_scop_from_pet_stmt(__isl_take isl_space *space,
	struct pet_stmt *stmt)
{
	struct pet_scop *scop;

	if (!stmt)
		space = isl_space_free(space);

	scop = scop_alloc(space, 1);
	if (!scop)
		goto error;

	scop->context = stmt_extract_context(stmt, scop->context);
	if (!scop->context)
		goto error;

	scop->stmts[0] = stmt;
	scop->loc = pet_loc_copy(stmt->loc);

	if (!scop->loc)
		return pet_scop_free(scop);

	return scop;
error:
	pet_stmt_free(stmt);
	pet_scop_free(scop);
	return NULL;
}

/* Does "mpa" represent an access to an element of an unnamed space, i.e.,
 * does it represent an affine expression?
 */
static int multi_pw_aff_is_affine(__isl_keep isl_multi_pw_aff *mpa)
{
	int has_id;

	has_id = isl_multi_pw_aff_has_tuple_id(mpa, isl_dim_out);
	if (has_id < 0)
		return -1;

	return !has_id;
}

/* Return the piecewise affine expression "set ? 1 : 0" defined on "dom".
 */
static __isl_give isl_pw_aff *indicator_function(__isl_take isl_set *set,
	__isl_take isl_set *dom)
{
	isl_pw_aff *pa;
	pa = isl_set_indicator_function(set);
	pa = isl_pw_aff_intersect_domain(pa, dom);
	return pa;
}

/* Return "lhs || rhs", defined on the shared definition domain.
 */
static __isl_give isl_pw_aff *pw_aff_or(__isl_take isl_pw_aff *lhs,
	__isl_take isl_pw_aff *rhs)
{
	isl_set *cond;
	isl_set *dom;

	dom = isl_set_intersect(isl_pw_aff_domain(isl_pw_aff_copy(lhs)),
				 isl_pw_aff_domain(isl_pw_aff_copy(rhs)));
	cond = isl_set_union(isl_pw_aff_non_zero_set(lhs),
			     isl_pw_aff_non_zero_set(rhs));
	cond = isl_set_coalesce(cond);
	return indicator_function(cond, dom);
}

/* Combine ext1->skip[type] and ext2->skip[type] into ext->skip[type].
 * ext may be equal to either ext1 or ext2.
 *
 * The two skips that need to be combined are assumed to be affine expressions.
 *
 * We need to skip in ext if we need to skip in either ext1 or ext2.
 * We don't need to skip in ext if we don't need to skip in both ext1 and ext2.
 */
static struct pet_scop_ext *combine_skips(struct pet_scop_ext *ext,
	struct pet_scop_ext *ext1, struct pet_scop_ext *ext2,
	enum pet_skip type)
{
	isl_pw_aff *skip, *skip1, *skip2;

	if (!ext)
		return NULL;
	if (!ext1->skip[type] && !ext2->skip[type])
		return ext;
	if (!ext1->skip[type]) {
		if (ext == ext2)
			return ext;
		ext->skip[type] = ext2->skip[type];
		ext2->skip[type] = NULL;
		return ext;
	}
	if (!ext2->skip[type]) {
		if (ext == ext1)
			return ext;
		ext->skip[type] = ext1->skip[type];
		ext1->skip[type] = NULL;
		return ext;
	}

	if (!multi_pw_aff_is_affine(ext1->skip[type]) ||
	    !multi_pw_aff_is_affine(ext2->skip[type]))
		isl_die(isl_multi_pw_aff_get_ctx(ext1->skip[type]),
			isl_error_internal, "can only combine affine skips",
			goto error);

	skip1 = isl_multi_pw_aff_get_pw_aff(ext1->skip[type], 0);
	skip2 = isl_multi_pw_aff_get_pw_aff(ext2->skip[type], 0);
	skip = pw_aff_or(skip1, skip2);
	isl_multi_pw_aff_free(ext1->skip[type]);
	ext1->skip[type] = NULL;
	isl_multi_pw_aff_free(ext2->skip[type]);
	ext2->skip[type] = NULL;
	ext->skip[type] = isl_multi_pw_aff_from_pw_aff(skip);
	if (!ext->skip[type])
		goto error;

	return ext;
error:
	pet_scop_free(&ext->scop);
	return NULL;
}

/* Combine scop1->skip[type] and scop2->skip[type] into scop->skip[type],
 * where type takes on the values pet_skip_now and pet_skip_later.
 * scop may be equal to either scop1 or scop2.
 */
static struct pet_scop *scop_combine_skips(struct pet_scop *scop,
	struct pet_scop *scop1, struct pet_scop *scop2)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;
	struct pet_scop_ext *ext1 = (struct pet_scop_ext *) scop1;
	struct pet_scop_ext *ext2 = (struct pet_scop_ext *) scop2;

	ext = combine_skips(ext, ext1, ext2, pet_skip_now);
	ext = combine_skips(ext, ext1, ext2, pet_skip_later);
	return &ext->scop;
}

/* Update start and end of scop->loc to include the region from "start"
 * to "end".  In particular, if scop->loc == &pet_loc_dummy, then "scop"
 * does not have any offset information yet and we simply take the information
 * from "start" and "end".  Otherwise, we update loc using "start" and "end".
 */
struct pet_scop *pet_scop_update_start_end(struct pet_scop *scop,
	unsigned start, unsigned end)
{
	if (!scop)
		return NULL;

	if (scop->loc == &pet_loc_dummy)
		scop->loc = pet_loc_alloc(isl_set_get_ctx(scop->context),
					    start, end, -1, strdup(""));
	else
		scop->loc = pet_loc_update_start_end(scop->loc, start, end);

	if (!scop->loc)
		return pet_scop_free(scop);

	return scop;
}

/* Update start and end of scop->loc to include the region identified
 * by "loc".
 */
struct pet_scop *pet_scop_update_start_end_from_loc(struct pet_scop *scop,
	__isl_keep pet_loc *loc)
{
	return pet_scop_update_start_end(scop, pet_loc_get_start(loc),
						pet_loc_get_end(loc));
}

/* Replace the location of "scop" by "loc".
 */
struct pet_scop *pet_scop_set_loc(struct pet_scop *scop,
	__isl_take pet_loc *loc)
{
	if (!scop || !loc)
		goto error;

	pet_loc_free(scop->loc);
	scop->loc = loc;

	return scop;
error:
	pet_loc_free(loc);
	pet_scop_free(scop);
	return NULL;
}

/* Does "implication" appear in the list of implications of "scop"?
 */
static int is_known_implication(struct pet_scop *scop,
	struct pet_implication *implication)
{
	int i;

	for (i = 0; i < scop->n_implication; ++i) {
		struct pet_implication *pi = scop->implications[i];
		int equal;

		if (pi->satisfied != implication->satisfied)
			continue;
		equal = isl_map_is_equal(pi->extension, implication->extension);
		if (equal < 0)
			return -1;
		if (equal)
			return 1;
	}

	return 0;
}

/* Store the concatenation of the implications of "scop1" and "scop2"
 * in "scop", removing duplicates (i.e., implications in "scop2" that
 * already appear in "scop1").
 */
static struct pet_scop *scop_collect_implications(isl_ctx *ctx,
	struct pet_scop *scop, struct pet_scop *scop1, struct pet_scop *scop2)
{
	int i, j;

	if (!scop)
		return NULL;

	if (scop2->n_implication == 0) {
		scop->n_implication = scop1->n_implication;
		scop->implications = scop1->implications;
		scop1->n_implication = 0;
		scop1->implications = NULL;
		return scop;
	}

	if (scop1->n_implication == 0) {
		scop->n_implication = scop2->n_implication;
		scop->implications = scop2->implications;
		scop2->n_implication = 0;
		scop2->implications = NULL;
		return scop;
	}

	scop->implications = isl_calloc_array(ctx, struct pet_implication *,
				scop1->n_implication + scop2->n_implication);
	if (!scop->implications)
		return pet_scop_free(scop);

	for (i = 0; i < scop1->n_implication; ++i) {
		scop->implications[i] = scop1->implications[i];
		scop1->implications[i] = NULL;
	}

	scop->n_implication = scop1->n_implication;
	j = scop1->n_implication;
	for (i = 0; i < scop2->n_implication; ++i) {
		int known;

		known = is_known_implication(scop, scop2->implications[i]);
		if (known < 0)
			return pet_scop_free(scop);
		if (known)
			continue;
		scop->implications[j++] = scop2->implications[i];
		scop2->implications[i] = NULL;
	}
	scop->n_implication = j;

	return scop;
}

/* Combine the offset information of "scop1" and "scop2" into "scop".
 */
static struct pet_scop *scop_combine_start_end(struct pet_scop *scop,
	struct pet_scop *scop1, struct pet_scop *scop2)
{
	if (scop1->loc != &pet_loc_dummy)
		scop = pet_scop_update_start_end_from_loc(scop, scop1->loc);
	if (scop2->loc != &pet_loc_dummy)
		scop = pet_scop_update_start_end_from_loc(scop, scop2->loc);
	return scop;
}

/* Construct a pet_scop that contains the offset information,
 * arrays, statements and skip information in "scop1" and "scop2".
 */
static struct pet_scop *pet_scop_add(isl_ctx *ctx, struct pet_scop *scop1,
	struct pet_scop *scop2)
{
	int i;
	isl_space *space;
	struct pet_scop *scop = NULL;

	if (!scop1 || !scop2)
		goto error;

	if (scop1->n_stmt == 0) {
		scop2 = scop_combine_skips(scop2, scop1, scop2);
		pet_scop_free(scop1);
		return scop2;
	}

	if (scop2->n_stmt == 0) {
		scop1 = scop_combine_skips(scop1, scop1, scop2);
		pet_scop_free(scop2);
		return scop1;
	}

	space = isl_set_get_space(scop1->context);
	scop = scop_alloc(space, scop1->n_stmt + scop2->n_stmt);
	if (!scop)
		goto error;

	scop->arrays = isl_calloc_array(ctx, struct pet_array *,
					scop1->n_array + scop2->n_array);
	if (!scop->arrays)
		goto error;
	scop->n_array = scop1->n_array + scop2->n_array;

	for (i = 0; i < scop1->n_stmt; ++i) {
		scop->stmts[i] = scop1->stmts[i];
		scop1->stmts[i] = NULL;
	}

	for (i = 0; i < scop2->n_stmt; ++i) {
		scop->stmts[scop1->n_stmt + i] = scop2->stmts[i];
		scop2->stmts[i] = NULL;
	}

	for (i = 0; i < scop1->n_array; ++i) {
		scop->arrays[i] = scop1->arrays[i];
		scop1->arrays[i] = NULL;
	}

	for (i = 0; i < scop2->n_array; ++i) {
		scop->arrays[scop1->n_array + i] = scop2->arrays[i];
		scop2->arrays[i] = NULL;
	}

	scop = scop_collect_implications(ctx, scop, scop1, scop2);
	scop = pet_scop_restrict_context(scop, isl_set_copy(scop1->context));
	scop = pet_scop_restrict_context(scop, isl_set_copy(scop2->context));
	scop = scop_combine_skips(scop, scop1, scop2);
	scop = scop_combine_start_end(scop, scop1, scop2);

	pet_scop_free(scop1);
	pet_scop_free(scop2);
	return scop;
error:
	pet_scop_free(scop1);
	pet_scop_free(scop2);
	pet_scop_free(scop);
	return NULL;
}

/* Apply the skip condition "skip" to "scop".
 * That is, make sure "scop" is not executed when the condition holds.
 *
 * If "skip" is an affine expression, we add the conditions under
 * which the expression is zero to the iteration domains.
 * Otherwise, we add a filter on the variable attaining the value zero.
 */
static struct pet_scop *restrict_skip(struct pet_scop *scop,
	__isl_take isl_multi_pw_aff *skip)
{
	isl_set *zero;
	isl_pw_aff *pa;
	int is_aff;

	if (!scop || !skip)
		goto error;

	is_aff = multi_pw_aff_is_affine(skip);
	if (is_aff < 0)
		goto error;

	if (!is_aff)
		return pet_scop_filter(scop, skip, 0);

	pa = isl_multi_pw_aff_get_pw_aff(skip, 0);
	isl_multi_pw_aff_free(skip);
	zero = isl_pw_aff_zero_set(pa);
	scop = pet_scop_restrict(scop, zero);

	return scop;
error:
	isl_multi_pw_aff_free(skip);
	return pet_scop_free(scop);
}

/* Construct a pet_scop that contains the arrays, statements and
 * skip information in "scop1" and "scop2", where the two scops
 * are executed "in sequence".  That is, breaks and continues
 * in scop1 have an effect on scop2.
 */
struct pet_scop *pet_scop_add_seq(isl_ctx *ctx, struct pet_scop *scop1,
	struct pet_scop *scop2)
{
	if (scop1 && pet_scop_has_skip(scop1, pet_skip_now))
		scop2 = restrict_skip(scop2,
					pet_scop_get_skip(scop1, pet_skip_now));
	return pet_scop_add(ctx, scop1, scop2);
}

/* Construct a pet_scop that contains the arrays, statements and
 * skip information in "scop1" and "scop2", where the two scops
 * are executed "in parallel".  That is, any break or continue
 * in scop1 has no effect on scop2.
 */
struct pet_scop *pet_scop_add_par(isl_ctx *ctx, struct pet_scop *scop1,
	struct pet_scop *scop2)
{
	return pet_scop_add(ctx, scop1, scop2);
}

void *pet_implication_free(struct pet_implication *implication)
{
	int i;

	if (!implication)
		return NULL;

	isl_map_free(implication->extension);

	free(implication);
	return NULL;
}

struct pet_scop *pet_scop_free(struct pet_scop *scop)
{
	int i;
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return NULL;
	pet_loc_free(scop->loc);
	isl_set_free(scop->context);
	isl_set_free(scop->context_value);
	if (scop->types)
		for (i = 0; i < scop->n_type; ++i)
			pet_type_free(scop->types[i]);
	free(scop->types);
	if (scop->arrays)
		for (i = 0; i < scop->n_array; ++i)
			pet_array_free(scop->arrays[i]);
	free(scop->arrays);
	if (scop->stmts)
		for (i = 0; i < scop->n_stmt; ++i)
			pet_stmt_free(scop->stmts[i]);
	free(scop->stmts);
	if (scop->implications)
		for (i = 0; i < scop->n_implication; ++i)
			pet_implication_free(scop->implications[i]);
	free(scop->implications);
	isl_multi_pw_aff_free(ext->skip[pet_skip_now]);
	isl_multi_pw_aff_free(ext->skip[pet_skip_later]);
	free(scop);
	return NULL;
}

void pet_type_dump(struct pet_type *type)
{
	if (!type)
		return;

	fprintf(stderr, "%s -> %s\n", type->name, type->definition);
}

void pet_implication_dump(struct pet_implication *implication)
{
	if (!implication)
		return;

	fprintf(stderr, "%d\n", implication->satisfied);
	isl_map_dump(implication->extension);
}

void pet_scop_dump(struct pet_scop *scop)
{
	int i;
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return;
	
	isl_set_dump(scop->context);
	isl_set_dump(scop->context_value);
	for (i = 0; i < scop->n_type; ++i)
		pet_type_dump(scop->types[i]);
	for (i = 0; i < scop->n_array; ++i)
		pet_array_dump(scop->arrays[i]);
	for (i = 0; i < scop->n_stmt; ++i)
		pet_stmt_dump(scop->stmts[i]);
	for (i = 0; i < scop->n_implication; ++i)
		pet_implication_dump(scop->implications[i]);

	if (ext->skip[0]) {
		fprintf(stderr, "skip\n");
		isl_multi_pw_aff_dump(ext->skip[0]);
		isl_multi_pw_aff_dump(ext->skip[1]);
	}
}

/* Return 1 if the two pet_arrays are equivalent.
 *
 * We don't compare element_size as this may be target dependent.
 */
int pet_array_is_equal(struct pet_array *array1, struct pet_array *array2)
{
	if (!array1 || !array2)
		return 0;

	if (!isl_set_is_equal(array1->context, array2->context))
		return 0;
	if (!isl_set_is_equal(array1->extent, array2->extent))
		return 0;
	if (!!array1->value_bounds != !!array2->value_bounds)
		return 0;
	if (array1->value_bounds &&
	    !isl_set_is_equal(array1->value_bounds, array2->value_bounds))
		return 0;
	if (strcmp(array1->element_type, array2->element_type))
		return 0;
	if (array1->element_is_record != array2->element_is_record)
		return 0;
	if (array1->live_out != array2->live_out)
		return 0;
	if (array1->uniquely_defined != array2->uniquely_defined)
		return 0;
	if (array1->declared != array2->declared)
		return 0;
	if (array1->exposed != array2->exposed)
		return 0;

	return 1;
}

/* Return 1 if the two pet_stmts are equivalent.
 */
int pet_stmt_is_equal(struct pet_stmt *stmt1, struct pet_stmt *stmt2)
{
	int i;

	if (!stmt1 || !stmt2)
		return 0;
	
	if (pet_loc_get_line(stmt1->loc) != pet_loc_get_line(stmt2->loc))
		return 0;
	if (!isl_set_is_equal(stmt1->domain, stmt2->domain))
		return 0;
	if (!isl_map_is_equal(stmt1->schedule, stmt2->schedule))
		return 0;
	if (!pet_expr_is_equal(stmt1->body, stmt2->body))
		return 0;
	if (stmt1->n_arg != stmt2->n_arg)
		return 0;
	for (i = 0; i < stmt1->n_arg; ++i) {
		if (!pet_expr_is_equal(stmt1->args[i], stmt2->args[i]))
			return 0;
	}

	return 1;
}

/* Return 1 if the two pet_types are equivalent.
 *
 * We only compare the names of the types since the exact representation
 * of the definition may depend on the version of clang being used.
 */
int pet_type_is_equal(struct pet_type *type1, struct pet_type *type2)
{
	if (!type1 || !type2)
		return 0;

	if (strcmp(type1->name, type2->name))
		return 0;

	return 1;
}

/* Return 1 if the two pet_implications are equivalent.
 */
int pet_implication_is_equal(struct pet_implication *implication1,
	struct pet_implication *implication2)
{
	if (!implication1 || !implication2)
		return 0;

	if (implication1->satisfied != implication2->satisfied)
		return 0;
	if (!isl_map_is_equal(implication1->extension, implication2->extension))
		return 0;

	return 1;
}

/* Return 1 if the two pet_scops are equivalent.
 */
int pet_scop_is_equal(struct pet_scop *scop1, struct pet_scop *scop2)
{
	int i;

	if (!scop1 || !scop2)
		return 0;

	if (!isl_set_is_equal(scop1->context, scop2->context))
		return 0;
	if (!isl_set_is_equal(scop1->context_value, scop2->context_value))
		return 0;

	if (scop1->n_type != scop2->n_type)
		return 0;
	for (i = 0; i < scop1->n_type; ++i)
		if (!pet_type_is_equal(scop1->types[i], scop2->types[i]))
			return 0;

	if (scop1->n_array != scop2->n_array)
		return 0;
	for (i = 0; i < scop1->n_array; ++i)
		if (!pet_array_is_equal(scop1->arrays[i], scop2->arrays[i]))
			return 0;

	if (scop1->n_stmt != scop2->n_stmt)
		return 0;
	for (i = 0; i < scop1->n_stmt; ++i)
		if (!pet_stmt_is_equal(scop1->stmts[i], scop2->stmts[i]))
			return 0;

	if (scop1->n_implication != scop2->n_implication)
		return 0;
	for (i = 0; i < scop1->n_implication; ++i)
		if (!pet_implication_is_equal(scop1->implications[i],
						scop2->implications[i]))
			return 0;

	return 1;
}

/* Does the set "extent" reference a virtual array, i.e.,
 * one with user pointer equal to NULL?
 * A virtual array does not have any members.
 */
static int extent_is_virtual_array(__isl_keep isl_set *extent)
{
	isl_id *id;
	int is_virtual;

	if (!isl_set_has_tuple_id(extent))
		return 0;
	if (isl_set_is_wrapping(extent))
		return 0;
	id = isl_set_get_tuple_id(extent);
	is_virtual = !isl_id_get_user(id);
	isl_id_free(id);

	return is_virtual;
}

/* Intersect the initial dimensions of "array" with "domain", provided
 * that "array" represents a virtual array.
 *
 * If "array" is virtual, then We take the preimage of "domain"
 * over the projection of the extent of "array" onto its initial dimensions
 * and intersect this extent with the result.
 */
static struct pet_array *virtual_array_intersect_domain_prefix(
	struct pet_array *array, __isl_take isl_set *domain)
{
	int n;
	isl_space *space;
	isl_multi_aff *ma;

	if (!array || !extent_is_virtual_array(array->extent)) {
		isl_set_free(domain);
		return array;
	}

	space = isl_set_get_space(array->extent);
	n = isl_set_dim(domain, isl_dim_set);
	ma = pet_prefix_projection(space, n);
	domain = isl_set_preimage_multi_aff(domain, ma);

	array->extent = isl_set_intersect(array->extent, domain);
	if (!array->extent)
		return pet_array_free(array);

	return array;
}

/* Intersect the initial dimensions of the domain of "stmt"
 * with "domain".
 *
 * We take the preimage of "domain" over the projection of the
 * domain of "stmt" onto its initial dimensions and intersect
 * the domain of "stmt" with the result.
 */
static struct pet_stmt *stmt_intersect_domain_prefix(struct pet_stmt *stmt,
	__isl_take isl_set *domain)
{
	int n;
	isl_space *space;
	isl_multi_aff *ma;

	if (!stmt)
		goto error;

	space = isl_set_get_space(stmt->domain);
	n = isl_set_dim(domain, isl_dim_set);
	ma = pet_prefix_projection(space, n);
	domain = isl_set_preimage_multi_aff(domain, ma);

	stmt->domain = isl_set_intersect(stmt->domain, domain);
	if (!stmt->domain)
		return pet_stmt_free(stmt);

	return stmt;
error:
	isl_set_free(domain);
	return pet_stmt_free(stmt);
}

/* Intersect the initial dimensions of the domain of "implication"
 * with "domain".
 *
 * We take the preimage of "domain" over the projection of the
 * domain of "implication" onto its initial dimensions and intersect
 * the domain of "implication" with the result.
 */
static struct pet_implication *implication_intersect_domain_prefix(
	struct pet_implication *implication, __isl_take isl_set *domain)
{
	int n;
	isl_space *space;
	isl_multi_aff *ma;

	if (!implication)
		goto error;

	space = isl_map_get_space(implication->extension);
	n = isl_set_dim(domain, isl_dim_set);
	ma = pet_prefix_projection(isl_space_domain(space), n);
	domain = isl_set_preimage_multi_aff(domain, ma);

	implication->extension =
		isl_map_intersect_domain(implication->extension, domain);
	if (!implication->extension)
		return pet_implication_free(implication);

	return implication;
error:
	isl_set_free(domain);
	return pet_implication_free(implication);
}

/* Intersect the initial dimensions of the domains in "scop" with "domain".
 *
 * The extents of the virtual arrays match the iteration domains,
 * so if the iteration domain changes, we need to change those extents too.
 */
struct pet_scop *pet_scop_intersect_domain_prefix(struct pet_scop *scop,
	__isl_take isl_set *domain)
{
	int i;

	if (!scop)
		goto error;

	for (i = 0; i < scop->n_array; ++i) {
		scop->arrays[i] = virtual_array_intersect_domain_prefix(
					scop->arrays[i], isl_set_copy(domain));
		if (!scop->arrays[i])
			goto error;
	}

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = stmt_intersect_domain_prefix(scop->stmts[i],
						    isl_set_copy(domain));
		if (!scop->stmts[i])
			goto error;
	}

	for (i = 0; i < scop->n_implication; ++i) {
		scop->implications[i] =
		    implication_intersect_domain_prefix(scop->implications[i],
						    isl_set_copy(domain));
		if (!scop->implications[i])
			return pet_scop_free(scop);
	}

	isl_set_free(domain);
	return scop;
error:
	isl_set_free(domain);
	return pet_scop_free(scop);
}

/* Prefix the schedule of "stmt" with an extra dimension with constant
 * value "pos".
 */
struct pet_stmt *pet_stmt_prefix(struct pet_stmt *stmt, int pos)
{
	if (!stmt)
		return NULL;

	stmt->schedule = isl_map_insert_dims(stmt->schedule, isl_dim_out, 0, 1);
	stmt->schedule = isl_map_fix_si(stmt->schedule, isl_dim_out, 0, pos);
	if (!stmt->schedule)
		return pet_stmt_free(stmt);

	return stmt;
}

/* Prefix the schedules of all statements in "scop" with an extra
 * dimension with constant value "pos".
 */
struct pet_scop *pet_scop_prefix(struct pet_scop *scop, int pos)
{
	int i;

	if (!scop)
		return NULL;

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = pet_stmt_prefix(scop->stmts[i], pos);
		if (!scop->stmts[i])
			return pet_scop_free(scop);
	}

	return scop;
}

/* Prefix the schedule of "stmt" with "sched".
 *
 * The domain of "sched" refers the current outer loop iterators and
 * needs to be mapped to the iteration domain of "stmt" first
 * before being prepended to the schedule of "stmt".
 */
static struct pet_stmt *pet_stmt_embed(struct pet_stmt *stmt,
	__isl_take isl_map *sched)
{
	int n;
	isl_space *space;
	isl_multi_aff *ma;

	if (!stmt)
		goto error;

	space = pet_stmt_get_space(stmt);
	n = isl_map_dim(sched, isl_dim_in);
	ma = pet_prefix_projection(space, n);
	sched = isl_map_preimage_domain_multi_aff(sched, ma);
	stmt->schedule = isl_map_flat_range_product(sched, stmt->schedule);
	if (!stmt->schedule)
		return pet_stmt_free(stmt);

	return stmt;
error:
	isl_map_free(sched);
	return NULL;
}

/* Update the context with respect to an embedding into a loop
 * with iteration domain "dom".
 * The input context lives in the same space as "dom".
 * The output context has the inner dimension removed.
 *
 * An outer loop iterator value is invalid for the embedding if
 * any of the corresponding inner iterator values is invalid.
 * That is, an outer loop iterator value is valid only if all the corresponding
 * inner iterator values are valid.
 * We therefore compute the set of outer loop iterators l
 *
 *	forall i: dom(l,i) => valid(l,i)
 *
 * or
 *
 *	forall i: not dom(l,i) or valid(l,i)
 *
 * or
 *
 *	not exists i: dom(l,i) and not valid(l,i)
 *
 * i.e.,
 *
 *	not exists i: (dom \ valid)(l,i)
 *
 * If there are any unnamed parameters in "dom", then we consider
 * a parameter value to be valid if it is valid for any value of those
 * unnamed parameters.  They are therefore projected out at the end.
 */
static __isl_give isl_set *context_embed(__isl_take isl_set *context,
	__isl_keep isl_set *dom)
{
	int pos;

	pos = isl_set_dim(context, isl_dim_set) - 1;
	context = isl_set_subtract(isl_set_copy(dom), context);
	context = isl_set_project_out(context, isl_dim_set, pos, 1);
	context = isl_set_complement(context);
	context = pet_nested_remove_from_set(context);

	return context;
}

/* Update the implication with respect to an embedding into a loop
 * with iteration domain "dom".
 *
 * Since embed_access extends virtual arrays along with the domain
 * of the access, we need to do the same with domain and range
 * of the implication.  Since the original implication is only valid
 * within a given iteration of the loop, the extended implication
 * maps the extra array dimension corresponding to the extra loop
 * to itself.
 */
static struct pet_implication *pet_implication_embed(
	struct pet_implication *implication, __isl_take isl_set *dom)
{
	isl_id *id;
	isl_map *map;

	if (!implication)
		goto error;

	map = isl_set_identity(dom);
	id = isl_map_get_tuple_id(implication->extension, isl_dim_in);
	map = isl_map_flat_product(map, implication->extension);
	map = isl_map_set_tuple_id(map, isl_dim_in, isl_id_copy(id));
	map = isl_map_set_tuple_id(map, isl_dim_out, id);
	implication->extension = map;
	if (!implication->extension)
		return pet_implication_free(implication);

	return implication;
error:
	isl_set_free(dom);
	return NULL;
}

/* Adjust the context and statement schedules according to an embedding
 * in a loop with iteration domain "dom" and schedule "sched".
 *
 * Any skip conditions within the loop have no effect outside of the loop.
 * The caller is responsible for making sure skip[pet_skip_later] has been
 * taken into account.
 */
struct pet_scop *pet_scop_embed(struct pet_scop *scop, __isl_take isl_set *dom,
	__isl_take isl_aff *sched)
{
	int i;
	isl_map *sched_map;

	sched_map = isl_map_from_aff(sched);

	if (!scop)
		goto error;

	pet_scop_reset_skip(scop, pet_skip_now);
	pet_scop_reset_skip(scop, pet_skip_later);

	scop->context = context_embed(scop->context, dom);
	if (!scop->context)
		goto error;

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = pet_stmt_embed(scop->stmts[i],
				    isl_map_copy(sched_map));
		if (!scop->stmts[i])
			goto error;
	}

	isl_set_free(dom);
	isl_map_free(sched_map);
	return scop;
error:
	isl_set_free(dom);
	isl_map_free(sched_map);
	return pet_scop_free(scop);
}

/* Add extra conditions to scop->skip[type].
 *
 * The new skip condition only holds if it held before
 * and the condition is true.  It does not hold if it did not hold
 * before or the condition is false.
 *
 * The skip condition is assumed to be an affine expression.
 */
static struct pet_scop *pet_scop_restrict_skip(struct pet_scop *scop,
	enum pet_skip type, __isl_keep isl_set *cond)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;
	isl_pw_aff *skip;
	isl_set *dom;

	if (!scop)
		return NULL;
	if (!ext->skip[type])
		return scop;

	if (!multi_pw_aff_is_affine(ext->skip[type]))
		isl_die(isl_multi_pw_aff_get_ctx(ext->skip[type]),
			isl_error_internal, "can only restrict affine skips",
			return pet_scop_free(scop));

	skip = isl_multi_pw_aff_get_pw_aff(ext->skip[type], 0);
	dom = isl_pw_aff_domain(isl_pw_aff_copy(skip));
	cond = isl_set_copy(cond);
	cond = isl_set_intersect(cond, isl_pw_aff_non_zero_set(skip));
	skip = indicator_function(cond, dom);
	isl_multi_pw_aff_free(ext->skip[type]);
	ext->skip[type] = isl_multi_pw_aff_from_pw_aff(skip);
	if (!ext->skip[type])
		return pet_scop_free(scop);

	return scop;
}

/* Adjust the context and the skip conditions to the fact that
 * the scop was created in a context where "cond" holds.
 *
 * An outer loop iterator or parameter value is valid for the result
 * if it was valid for the original scop and satisfies "cond" or if it does
 * not satisfy "cond" as in this case the scop is not executed
 * and the original constraints on these values are irrelevant.
 */
struct pet_scop *pet_scop_restrict(struct pet_scop *scop,
	__isl_take isl_set *cond)
{
	int i;

	scop = pet_scop_restrict_skip(scop, pet_skip_now, cond);
	scop = pet_scop_restrict_skip(scop, pet_skip_later, cond);

	if (!scop)
		goto error;

	scop->context = isl_set_intersect(scop->context, isl_set_copy(cond));
	scop->context = isl_set_union(scop->context,
				isl_set_complement(isl_set_copy(cond)));
	scop->context = isl_set_coalesce(scop->context);
	scop->context = pet_nested_remove_from_set(scop->context);
	if (!scop->context)
		goto error;

	isl_set_free(cond);
	return scop;
error:
	isl_set_free(cond);
	return pet_scop_free(scop);
}

/* Insert an argument expression corresponding to "test" in front
 * of the list of arguments described by *n_arg and *args.
 */
static int args_insert_access(unsigned *n_arg, pet_expr ***args,
	__isl_keep isl_multi_pw_aff *test)
{
	int i;
	isl_ctx *ctx = isl_multi_pw_aff_get_ctx(test);

	if (!test)
		return -1;

	if (!*args) {
		*args = isl_calloc_array(ctx, pet_expr *, 1);
		if (!*args)
			return -1;
	} else {
		pet_expr **ext;
		ext = isl_calloc_array(ctx, pet_expr *, 1 + *n_arg);
		if (!ext)
			return -1;
		for (i = 0; i < *n_arg; ++i)
			ext[1 + i] = (*args)[i];
		free(*args);
		*args = ext;
	}
	(*n_arg)++;
	(*args)[0] = pet_expr_from_index(isl_multi_pw_aff_copy(test));
	if (!(*args)[0])
		return -1;

	return 0;
}

/* Look through the applications in "scop" for any that can be
 * applied to the filter expressed by "map" and "satisified".
 * If there is any, then apply it to "map" and return the result.
 * Otherwise, return "map".
 * "id" is the identifier of the virtual array.
 *
 * We only introduce at most one implication for any given virtual array,
 * so we can apply the implication and return as soon as we find one.
 */
static __isl_give isl_map *apply_implications(struct pet_scop *scop,
	__isl_take isl_map *map, __isl_keep isl_id *id, int satisfied)
{
	int i;

	for (i = 0; i < scop->n_implication; ++i) {
		struct pet_implication *pi = scop->implications[i];
		isl_id *pi_id;

		if (pi->satisfied != satisfied)
			continue;
		pi_id = isl_map_get_tuple_id(pi->extension, isl_dim_in);
		isl_id_free(pi_id);
		if (pi_id != id)
			continue;

		return isl_map_apply_range(map, isl_map_copy(pi->extension));
	}

	return map;
}

/* Is the filter expressed by "test" and "satisfied" implied
 * by filter "pos" on "domain", with filter "expr", taking into
 * account the implications of "scop"?
 *
 * For filter on domain implying that expressed by "test" and "satisfied",
 * the filter needs to be an access to the same (virtual) array as "test" and
 * the filter value needs to be equal to "satisfied".
 * Moreover, the filter access relation, possibly extended by
 * the implications in "scop" needs to contain "test".
 */
static int implies_filter(struct pet_scop *scop,
	__isl_keep isl_map *domain, int pos, __isl_keep pet_expr *expr,
	__isl_keep isl_map *test, int satisfied)
{
	isl_id *test_id, *arg_id;
	isl_val *val;
	int is_int;
	int s;
	int is_subset;
	isl_map *implied;

	if (expr->type != pet_expr_access)
		return 0;
	test_id = isl_map_get_tuple_id(test, isl_dim_out);
	arg_id = pet_expr_access_get_id(expr);
	isl_id_free(arg_id);
	isl_id_free(test_id);
	if (test_id != arg_id)
		return 0;
	val = isl_map_plain_get_val_if_fixed(domain, isl_dim_out, pos);
	is_int = isl_val_is_int(val);
	if (is_int)
		s = isl_val_get_num_si(val);
	isl_val_free(val);
	if (!val)
		return -1;
	if (!is_int)
		return 0;
	if (s != satisfied)
		return 0;

	implied = isl_map_copy(expr->acc.access);
	implied = apply_implications(scop, implied, test_id, satisfied);
	is_subset = isl_map_is_subset(test, implied);
	isl_map_free(implied);

	return is_subset;
}

/* Is the filter expressed by "test" and "satisfied" implied
 * by any of the filters on the domain of "stmt", taking into
 * account the implications of "scop"?
 */
static int filter_implied(struct pet_scop *scop,
	struct pet_stmt *stmt, __isl_keep isl_multi_pw_aff *test, int satisfied)
{
	int i;
	int implied;
	isl_id *test_id;
	isl_map *domain;
	isl_map *test_map;

	if (!scop || !stmt || !test)
		return -1;
	if (scop->n_implication == 0)
		return 0;
	if (stmt->n_arg == 0)
		return 0;

	domain = isl_set_unwrap(isl_set_copy(stmt->domain));
	test_map = isl_map_from_multi_pw_aff(isl_multi_pw_aff_copy(test));

	implied = 0;
	for (i = 0; i < stmt->n_arg; ++i) {
		implied = implies_filter(scop, domain, i, stmt->args[i],
					 test_map, satisfied);
		if (implied < 0 || implied)
			break;
	}

	isl_map_free(test_map);
	isl_map_free(domain);
	return implied;
}

/* Make the statement "stmt" depend on the value of "test"
 * being equal to "satisfied" by adjusting stmt->domain.
 *
 * The domain of "test" corresponds to the (zero or more) outer dimensions
 * of the iteration domain.
 *
 * We first extend "test" to apply to the entire iteration domain and
 * then check if the filter that we are about to add is implied
 * by any of the current filters, possibly taking into account
 * the implications in "scop".  If so, we leave "stmt" untouched and return.
 *
 * Otherwise, we insert an argument corresponding to a read to "test"
 * from the iteration domain of "stmt" in front of the list of arguments.
 * We also insert a corresponding output dimension in the wrapped
 * map contained in stmt->domain, with value set to "satisfied".
 */
static struct pet_stmt *stmt_filter(struct pet_scop *scop,
	struct pet_stmt *stmt, __isl_take isl_multi_pw_aff *test, int satisfied)
{
	int i;
	int implied;
	isl_id *id;
	isl_ctx *ctx;
	isl_pw_multi_aff *pma;
	isl_multi_aff *add_dom;
	isl_space *space;
	isl_local_space *ls;
	int n_test_dom;

	if (!stmt || !test)
		goto error;

	space = pet_stmt_get_space(stmt);
	n_test_dom = isl_multi_pw_aff_dim(test, isl_dim_in);
	space = isl_space_from_domain(space);
	space = isl_space_add_dims(space, isl_dim_out, n_test_dom);
	add_dom = isl_multi_aff_zero(isl_space_copy(space));
	ls = isl_local_space_from_space(isl_space_domain(space));
	for (i = 0; i < n_test_dom; ++i) {
		isl_aff *aff;
		aff = isl_aff_var_on_domain(isl_local_space_copy(ls),
					    isl_dim_set, i);
		add_dom = isl_multi_aff_set_aff(add_dom, i, aff);
	}
	isl_local_space_free(ls);
	test = isl_multi_pw_aff_pullback_multi_aff(test, add_dom);

	implied = filter_implied(scop, stmt, test, satisfied);
	if (implied < 0)
		goto error;
	if (implied) {
		isl_multi_pw_aff_free(test);
		return stmt;
	}

	id = isl_multi_pw_aff_get_tuple_id(test, isl_dim_out);
	pma = pet_filter_insert_pma(isl_set_get_space(stmt->domain),
					id, satisfied);
	stmt->domain = isl_set_preimage_pw_multi_aff(stmt->domain, pma);

	if (args_insert_access(&stmt->n_arg, &stmt->args, test) < 0)
		goto error;

	isl_multi_pw_aff_free(test);
	return stmt;
error:
	isl_multi_pw_aff_free(test);
	return pet_stmt_free(stmt);
}

/* Does "scop" have a skip condition of the given "type"?
 */
int pet_scop_has_skip(struct pet_scop *scop, enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return -1;
	return ext->skip[type] != NULL;
}

/* Does "scop" have a skip condition of the given "type" that
 * is an affine expression?
 */
int pet_scop_has_affine_skip(struct pet_scop *scop, enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return -1;
	if (!ext->skip[type])
		return 0;
	return multi_pw_aff_is_affine(ext->skip[type]);
}

/* Does "scop" have a skip condition of the given "type" that
 * is not an affine expression?
 */
int pet_scop_has_var_skip(struct pet_scop *scop, enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;
	int aff;

	if (!scop)
		return -1;
	if (!ext->skip[type])
		return 0;
	aff = multi_pw_aff_is_affine(ext->skip[type]);
	if (aff < 0)
		return -1;
	return !aff;
}

/* Does "scop" have a skip condition of the given "type" that
 * is affine and holds on the entire domain?
 */
int pet_scop_has_universal_skip(struct pet_scop *scop, enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;
	isl_pw_aff *pa;
	isl_set *set;
	int is_aff;
	int is_univ;

	is_aff = pet_scop_has_affine_skip(scop, type);
	if (is_aff < 0 || !is_aff)
		return is_aff;

	pa = isl_multi_pw_aff_get_pw_aff(ext->skip[type], 0);
	set = isl_pw_aff_non_zero_set(pa);
	is_univ = isl_set_plain_is_universe(set);
	isl_set_free(set);

	return is_univ;
}

/* Replace scop->skip[type] by "skip".
 */
struct pet_scop *pet_scop_set_skip(struct pet_scop *scop,
	enum pet_skip type, __isl_take isl_multi_pw_aff *skip)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop || !skip)
		goto error;

	isl_multi_pw_aff_free(ext->skip[type]);
	ext->skip[type] = skip;

	return scop;
error:
	isl_multi_pw_aff_free(skip);
	return pet_scop_free(scop);
}

/* Return a copy of scop->skip[type].
 */
__isl_give isl_multi_pw_aff *pet_scop_get_skip(struct pet_scop *scop,
	enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return NULL;

	return isl_multi_pw_aff_copy(ext->skip[type]);
}

/* Assuming scop->skip[type] is an affine expression,
 * return the constraints on the outer loop domain for which the skip condition
 * holds.
 */
__isl_give isl_set *pet_scop_get_affine_skip_domain(struct pet_scop *scop,
	enum pet_skip type)
{
	isl_multi_pw_aff *skip;
	isl_pw_aff *pa;

	skip = pet_scop_get_skip(scop, type);
	pa = isl_multi_pw_aff_get_pw_aff(skip, 0);
	isl_multi_pw_aff_free(skip);
	return isl_pw_aff_non_zero_set(pa);
}

/* Return the identifier of the variable that is accessed by
 * the skip condition of the given type.
 *
 * The skip condition is assumed not to be an affine condition.
 */
__isl_give isl_id *pet_scop_get_skip_id(struct pet_scop *scop,
	enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return NULL;

	return isl_multi_pw_aff_get_tuple_id(ext->skip[type], isl_dim_out);
}

/* Return an access pet_expr corresponding to the skip condition
 * of the given type.
 */
__isl_give pet_expr *pet_scop_get_skip_expr(struct pet_scop *scop,
	enum pet_skip type)
{
	return pet_expr_from_index(pet_scop_get_skip(scop, type));
}

/* Drop the the skip condition scop->skip[type].
 */
void pet_scop_reset_skip(struct pet_scop *scop, enum pet_skip type)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return;

	isl_multi_pw_aff_free(ext->skip[type]);
	ext->skip[type] = NULL;
}

/* Make the skip condition (if any) depend on the value of "test" being
 * equal to "satisfied".
 *
 * We only support the case where the original skip condition is universal,
 * i.e., where skipping is unconditional, and where satisfied == 1.
 * In this case, the skip condition is changed to skip only when 
 * "test" is equal to one.
 */
static struct pet_scop *pet_scop_filter_skip(struct pet_scop *scop,
	enum pet_skip type, __isl_keep isl_multi_pw_aff *test, int satisfied)
{
	int is_univ = 0;

	if (!scop)
		return NULL;
	if (!pet_scop_has_skip(scop, type))
		return scop;

	if (satisfied)
		is_univ = pet_scop_has_universal_skip(scop, type);
	if (is_univ < 0)
		return pet_scop_free(scop);
	if (satisfied && is_univ) {
		isl_multi_pw_aff *skip;
		skip = isl_multi_pw_aff_copy(test);
		scop = pet_scop_set_skip(scop, type, skip);
		if (!scop)
			return NULL;
	} else {
		isl_die(isl_multi_pw_aff_get_ctx(test), isl_error_internal,
			"skip expression cannot be filtered",
			return pet_scop_free(scop));
	}

	return scop;
}

/* Make all statements in "scop" depend on the value of "test"
 * being equal to "satisfied" by adjusting their domains.
 */
struct pet_scop *pet_scop_filter(struct pet_scop *scop,
	__isl_take isl_multi_pw_aff *test, int satisfied)
{
	int i;

	scop = pet_scop_filter_skip(scop, pet_skip_now, test, satisfied);
	scop = pet_scop_filter_skip(scop, pet_skip_later, test, satisfied);

	if (!scop || !test)
		goto error;

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = stmt_filter(scop, scop->stmts[i],
					isl_multi_pw_aff_copy(test), satisfied);
		if (!scop->stmts[i])
			goto error;
	}

	isl_multi_pw_aff_free(test);
	return scop;
error:
	isl_multi_pw_aff_free(test);
	return pet_scop_free(scop);
}

/* Add the parameters of the access expression "expr" to "space".
 */
static int access_collect_params(__isl_keep pet_expr *expr, void *user)
{
	int i;
	isl_space **space = user;

	*space = isl_space_align_params(*space,
					isl_map_get_space(expr->acc.access));

	return *space ? 0 : -1;
}

/* Add all parameters in "stmt" to "space" and return the result.
 */
static __isl_give isl_space *stmt_collect_params(struct pet_stmt *stmt,
	__isl_take isl_space *space)
{
	int i;

	if (!stmt)
		return isl_space_free(space);

	space = isl_space_align_params(space, isl_set_get_space(stmt->domain));
	space = isl_space_align_params(space,
					isl_map_get_space(stmt->schedule));
	for (i = 0; i < stmt->n_arg; ++i)
		if (pet_expr_foreach_access_expr(stmt->args[i],
					&access_collect_params, &space) < 0)
			space = isl_space_free(space);
	if (pet_expr_foreach_access_expr(stmt->body, &access_collect_params,
					&space) < 0)
		space = isl_space_free(space);

	return space;
}

/* Add all parameters in "array" to "space" and return the result.
 */
static __isl_give isl_space *array_collect_params(struct pet_array *array,
	__isl_take isl_space *space)
{
	if (!array)
		return isl_space_free(space);

	space = isl_space_align_params(space,
					isl_set_get_space(array->context));
	space = isl_space_align_params(space, isl_set_get_space(array->extent));

	return space;
}

/* Add all parameters in "scop" to "space" and return the result.
 */
static __isl_give isl_space *scop_collect_params(struct pet_scop *scop,
	__isl_take isl_space *space)
{
	int i;

	if (!scop)
		return isl_space_free(space);

	for (i = 0; i < scop->n_array; ++i)
		space = array_collect_params(scop->arrays[i], space);

	for (i = 0; i < scop->n_stmt; ++i)
		space = stmt_collect_params(scop->stmts[i], space);

	return space;
}

/* Add all parameters in "space" to the domain, schedule and
 * all access relations in "stmt".
 */
static struct pet_stmt *stmt_propagate_params(struct pet_stmt *stmt,
	__isl_take isl_space *space)
{
	int i;

	if (!stmt)
		goto error;

	stmt->domain = isl_set_align_params(stmt->domain,
						isl_space_copy(space));
	stmt->schedule = isl_map_align_params(stmt->schedule,
						isl_space_copy(space));

	for (i = 0; i < stmt->n_arg; ++i) {
		stmt->args[i] = pet_expr_align_params(stmt->args[i],
						isl_space_copy(space));
		if (!stmt->args[i])
			goto error;
	}
	stmt->body = pet_expr_align_params(stmt->body, isl_space_copy(space));

	if (!stmt->domain || !stmt->schedule || !stmt->body)
		goto error;

	isl_space_free(space);
	return stmt;
error:
	isl_space_free(space);
	return pet_stmt_free(stmt);
}

/* Add all parameters in "space" to "array".
 */
static struct pet_array *array_propagate_params(struct pet_array *array,
	__isl_take isl_space *space)
{
	if (!array)
		goto error;

	array->context = isl_set_align_params(array->context,
						isl_space_copy(space));
	array->extent = isl_set_align_params(array->extent,
						isl_space_copy(space));
	if (array->value_bounds) {
		array->value_bounds = isl_set_align_params(array->value_bounds,
							isl_space_copy(space));
		if (!array->value_bounds)
			goto error;
	}

	if (!array->context || !array->extent)
		goto error;

	isl_space_free(space);
	return array;
error:
	isl_space_free(space);
	return pet_array_free(array);
}

/* Add all parameters in "space" to "scop".
 */
static struct pet_scop *scop_propagate_params(struct pet_scop *scop,
	__isl_take isl_space *space)
{
	int i;

	if (!scop)
		goto error;

	for (i = 0; i < scop->n_array; ++i) {
		scop->arrays[i] = array_propagate_params(scop->arrays[i],
							isl_space_copy(space));
		if (!scop->arrays[i])
			goto error;
	}

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = stmt_propagate_params(scop->stmts[i],
							isl_space_copy(space));
		if (!scop->stmts[i])
			goto error;
	}

	isl_space_free(space);
	return scop;
error:
	isl_space_free(space);
	return pet_scop_free(scop);
}

/* Update all isl_sets and isl_maps in "scop" such that they all
 * have the same parameters.
 */
struct pet_scop *pet_scop_align_params(struct pet_scop *scop)
{
	isl_space *space;

	if (!scop)
		return NULL;

	space = isl_set_get_space(scop->context);
	space = scop_collect_params(scop, space);

	scop->context = isl_set_align_params(scop->context,
						isl_space_copy(space));
	scop = scop_propagate_params(scop, space);

	if (scop && !scop->context)
		return pet_scop_free(scop);

	return scop;
}

/* Add the access relation of the access expression "expr" to "accesses" and
 * return the result.
 * The domain of the access relation is intersected with "domain".
 * If "tag" is set, then the access relation is tagged with
 * the corresponding reference identifier.
 */
static __isl_give isl_union_map *expr_collect_access(__isl_keep pet_expr *expr,
	int tag, __isl_take isl_union_map *accesses, __isl_keep isl_set *domain)
{
	isl_map *access;

	access = pet_expr_access_get_may_access(expr);
	access = isl_map_intersect_domain(access, isl_set_copy(domain));
	if (tag)
		access = pet_expr_tag_access(expr, access);
	return isl_union_map_add_map(accesses, access);
}

/* Add all read access relations (if "read" is set) and/or all write
 * access relations (if "write" is set) to "accesses" and return the result.
 * The domains of the access relations are intersected with "domain".
 * If "tag" is set, then the access relations are tagged with
 * the corresponding reference identifiers.
 *
 * If "must" is set, then we only add the accesses that are definitely
 * performed.  Otherwise, we add all potential accesses.
 * In particular, if the access has any arguments, then if "must" is
 * set we currently skip the access completely.  If "must" is not set,
 * we project out the values of the access arguments.
 */
static __isl_give isl_union_map *expr_collect_accesses(
	__isl_keep pet_expr *expr, int read, int write, int must, int tag,
	__isl_take isl_union_map *accesses, __isl_keep isl_set *domain)
{
	int i;
	isl_id *id;
	isl_space *dim;

	if (!expr)
		return isl_union_map_free(accesses);

	for (i = 0; i < expr->n_arg; ++i)
		accesses = expr_collect_accesses(expr->args[i],
				     read, write, must, tag, accesses, domain);

	if (expr->type == pet_expr_access && !pet_expr_is_affine(expr) &&
	    ((read && expr->acc.read) || (write && expr->acc.write)) &&
	    (!must || expr->n_arg == 0)) {
		accesses = expr_collect_access(expr, tag, accesses, domain);
	}

	return accesses;
}

/* Collect and return all read access relations (if "read" is set)
 * and/or all write access relations (if "write" is set) in "stmt".
 * If "tag" is set, then the access relations are tagged with
 * the corresponding reference identifiers.
 * If "kill" is set, then "stmt" is a kill statement and we simply
 * add the argument of the kill operation.
 *
 * If "must" is set, then we only add the accesses that are definitely
 * performed.  Otherwise, we add all potential accesses.
 * In particular, if the statement has any arguments, then if "must" is
 * set we currently skip the statement completely.  If "must" is not set,
 * we project out the values of the statement arguments.
 */
static __isl_give isl_union_map *stmt_collect_accesses(struct pet_stmt *stmt,
	int read, int write, int kill, int must, int tag,
	__isl_take isl_space *dim)
{
	isl_union_map *accesses;
	isl_set *domain;

	if (!stmt)
		return NULL;

	accesses = isl_union_map_empty(dim);

	if (must && stmt->n_arg > 0)
		return accesses;

	domain = isl_set_copy(stmt->domain);
	if (isl_set_is_wrapping(domain))
		domain = isl_map_domain(isl_set_unwrap(domain));

	if (kill)
		accesses = expr_collect_access(stmt->body->args[0], tag,
						accesses, domain);
	else
		accesses = expr_collect_accesses(stmt->body, read, write,
						must, tag, accesses, domain);
	isl_set_free(domain);

	return accesses;
}

/* Is "stmt" an assignment statement?
 */
int pet_stmt_is_assign(struct pet_stmt *stmt)
{
	if (!stmt)
		return 0;
	if (stmt->body->type != pet_expr_op)
		return 0;
	return stmt->body->op == pet_op_assign;
}

/* Is "stmt" a kill statement?
 */
int pet_stmt_is_kill(struct pet_stmt *stmt)
{
	if (!stmt)
		return 0;
	if (stmt->body->type != pet_expr_op)
		return 0;
	return stmt->body->op == pet_op_kill;
}

/* Is "stmt" an assume statement?
 */
int pet_stmt_is_assume(struct pet_stmt *stmt)
{
	if (!stmt)
		return 0;
	return pet_expr_is_assume(stmt->body);
}

/* Compute a mapping from all arrays (of structs) in scop
 * to their innermost arrays.
 *
 * In particular, for each array of a primitive type, the result
 * contains the identity mapping on that array.
 * For each array involving member accesses, the result
 * contains a mapping from the elements of any intermediate array of structs
 * to all corresponding elements of the innermost nested arrays.
 */
static __isl_give isl_union_map *compute_to_inner(struct pet_scop *scop)
{
	int i;
	isl_union_map *to_inner;

	to_inner = isl_union_map_empty(isl_set_get_space(scop->context));

	for (i = 0; i < scop->n_array; ++i) {
		struct pet_array *array = scop->arrays[i];
		isl_set *set;
		isl_map *map, *gist;

		if (array->element_is_record)
			continue;

		map = isl_set_identity(isl_set_copy(array->extent));

		set = isl_map_domain(isl_map_copy(map));
		gist = isl_map_copy(map);
		gist = isl_map_gist_domain(gist, isl_set_copy(set));
		to_inner = isl_union_map_add_map(to_inner, gist);

		while (set && isl_set_is_wrapping(set)) {
			isl_id *id;
			isl_map *wrapped;

			id = isl_set_get_tuple_id(set);
			wrapped = isl_set_unwrap(set);
			wrapped = isl_map_domain_map(wrapped);
			wrapped = isl_map_set_tuple_id(wrapped, isl_dim_in, id);
			map = isl_map_apply_domain(map, wrapped);
			set = isl_map_domain(isl_map_copy(map));
			gist = isl_map_copy(map);
			gist = isl_map_gist_domain(gist, isl_set_copy(set));
			to_inner = isl_union_map_add_map(to_inner, gist);
		}

		isl_set_free(set);
		isl_map_free(map);
	}

	return to_inner;
}

/* Collect and return all read access relations (if "read" is set)
 * and/or all write access relations (if "write" is set) in "scop".
 * If "kill" is set, then we only add the arguments of kill operations.
 * If "must" is set, then we only add the accesses that are definitely
 * performed.  Otherwise, we add all potential accesses.
 * If "tag" is set, then the access relations are tagged with
 * the corresponding reference identifiers.
 * For accesses to structures, the returned access relation accesses
 * all individual fields in the structures.
 */
static __isl_give isl_union_map *scop_collect_accesses(struct pet_scop *scop,
	int read, int write, int kill, int must, int tag)
{
	int i;
	isl_union_map *accesses;
	isl_union_set *arrays;
	isl_union_map *to_inner;

	if (!scop)
		return NULL;

	accesses = isl_union_map_empty(isl_set_get_space(scop->context));

	for (i = 0; i < scop->n_stmt; ++i) {
		struct pet_stmt *stmt = scop->stmts[i];
		isl_union_map *accesses_i;
		isl_space *space;

		if (kill && !pet_stmt_is_kill(stmt))
			continue;

		space = isl_set_get_space(scop->context);
		accesses_i = stmt_collect_accesses(stmt, read, write, kill,
							must, tag, space);
		accesses = isl_union_map_union(accesses, accesses_i);
	}

	arrays = isl_union_set_empty(isl_union_map_get_space(accesses));
	for (i = 0; i < scop->n_array; ++i) {
		isl_set *extent = isl_set_copy(scop->arrays[i]->extent);
		arrays = isl_union_set_add_set(arrays, extent);
	}
	accesses = isl_union_map_intersect_range(accesses, arrays);

	to_inner = compute_to_inner(scop);
	accesses = isl_union_map_apply_range(accesses, to_inner);

	return accesses;
}

/* Collect all potential read access relations.
 */
__isl_give isl_union_map *pet_scop_collect_may_reads(struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 1, 0, 0, 0, 0);
}

/* Collect all potential write access relations.
 */
__isl_give isl_union_map *pet_scop_collect_may_writes(struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 0, 1, 0, 0, 0);
}

/* Collect all definite write access relations.
 */
__isl_give isl_union_map *pet_scop_collect_must_writes(struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 0, 1, 0, 1, 0);
}

/* Collect all definite kill access relations.
 */
__isl_give isl_union_map *pet_scop_collect_must_kills(struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 0, 0, 1, 1, 0);
}

/* Collect all tagged potential read access relations.
 */
__isl_give isl_union_map *pet_scop_collect_tagged_may_reads(
	struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 1, 0, 0, 0, 1);
}

/* Collect all tagged potential write access relations.
 */
__isl_give isl_union_map *pet_scop_collect_tagged_may_writes(
	struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 0, 1, 0, 0, 1);
}

/* Collect all tagged definite write access relations.
 */
__isl_give isl_union_map *pet_scop_collect_tagged_must_writes(
	struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 0, 1, 0, 1, 1);
}

/* Collect all tagged definite kill access relations.
 */
__isl_give isl_union_map *pet_scop_collect_tagged_must_kills(
	struct pet_scop *scop)
{
	return scop_collect_accesses(scop, 0, 0, 1, 1, 1);
}

/* Collect and return the union of iteration domains in "scop".
 */
__isl_give isl_union_set *pet_scop_collect_domains(struct pet_scop *scop)
{
	int i;
	isl_set *domain_i;
	isl_union_set *domain;

	if (!scop)
		return NULL;

	domain = isl_union_set_empty(isl_set_get_space(scop->context));

	for (i = 0; i < scop->n_stmt; ++i) {
		domain_i = isl_set_copy(scop->stmts[i]->domain);
		domain = isl_union_set_add_set(domain, domain_i);
	}

	return domain;
}

/* Collect and return the schedules of the statements in "scop".
 * The range is normalized to the maximal number of scheduling
 * dimensions.
 */
__isl_give isl_union_map *pet_scop_collect_schedule(struct pet_scop *scop)
{
	int i, j;
	isl_map *schedule_i;
	isl_union_map *schedule;
	int depth, max_depth = 0;

	if (!scop)
		return NULL;

	schedule = isl_union_map_empty(isl_set_get_space(scop->context));

	for (i = 0; i < scop->n_stmt; ++i) {
		depth = isl_map_dim(scop->stmts[i]->schedule, isl_dim_out);
		if (depth > max_depth)
			max_depth = depth;
	}

	for (i = 0; i < scop->n_stmt; ++i) {
		schedule_i = isl_map_copy(scop->stmts[i]->schedule);
		depth = isl_map_dim(schedule_i, isl_dim_out);
		schedule_i = isl_map_add_dims(schedule_i, isl_dim_out,
						max_depth - depth);
		for (j = depth; j < max_depth; ++j)
			schedule_i = isl_map_fix_si(schedule_i,
							isl_dim_out, j, 0);
		schedule = isl_union_map_add_map(schedule, schedule_i);
	}

	return schedule;
}

/* Add a reference identifier to all access expressions in "stmt".
 * "n_ref" points to an integer that contains the sequence number
 * of the next reference.
 */
static struct pet_stmt *stmt_add_ref_ids(struct pet_stmt *stmt, int *n_ref)
{
	int i;

	if (!stmt)
		return NULL;

	for (i = 0; i < stmt->n_arg; ++i) {
		stmt->args[i] = pet_expr_add_ref_ids(stmt->args[i], n_ref);
		if (!stmt->args[i])
			return pet_stmt_free(stmt);
	}

	stmt->body = pet_expr_add_ref_ids(stmt->body, n_ref);
	if (!stmt->body)
		return pet_stmt_free(stmt);

	return stmt;
}

/* Add a reference identifier to all access expressions in "scop".
 */
struct pet_scop *pet_scop_add_ref_ids(struct pet_scop *scop)
{
	int i;
	int n_ref;

	if (!scop)
		return NULL;

	n_ref = 0;
	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = stmt_add_ref_ids(scop->stmts[i], &n_ref);
		if (!scop->stmts[i])
			return pet_scop_free(scop);
	}

	return scop;
}

/* Reset the user pointer on all parameter ids in "array".
 */
static struct pet_array *array_anonymize(struct pet_array *array)
{
	if (!array)
		return NULL;

	array->context = isl_set_reset_user(array->context);
	array->extent = isl_set_reset_user(array->extent);
	if (!array->context || !array->extent)
		return pet_array_free(array);

	return array;
}

/* Reset the user pointer on all parameter and tuple ids in "stmt".
 */
static struct pet_stmt *stmt_anonymize(struct pet_stmt *stmt)
{
	int i;
	isl_space *space;
	isl_set *domain;

	if (!stmt)
		return NULL;

	stmt->domain = isl_set_reset_user(stmt->domain);
	stmt->schedule = isl_map_reset_user(stmt->schedule);
	if (!stmt->domain || !stmt->schedule)
		return pet_stmt_free(stmt);

	for (i = 0; i < stmt->n_arg; ++i) {
		stmt->args[i] = pet_expr_anonymize(stmt->args[i]);
		if (!stmt->args[i])
			return pet_stmt_free(stmt);
	}

	stmt->body = pet_expr_anonymize(stmt->body);
	if (!stmt->body)
		return pet_stmt_free(stmt);

	return stmt;
}

/* Reset the user pointer on the tuple ids and all parameter ids
 * in "implication".
 */
static struct pet_implication *implication_anonymize(
	struct pet_implication *implication)
{
	if (!implication)
		return NULL;

	implication->extension = isl_map_reset_user(implication->extension);
	if (!implication->extension)
		return pet_implication_free(implication);

	return implication;
}

/* Reset the user pointer on all parameter and tuple ids in "scop".
 */
struct pet_scop *pet_scop_anonymize(struct pet_scop *scop)
{
	int i;

	if (!scop)
		return NULL;

	scop->context = isl_set_reset_user(scop->context);
	scop->context_value = isl_set_reset_user(scop->context_value);
	if (!scop->context || !scop->context_value)
		return pet_scop_free(scop);

	for (i = 0; i < scop->n_array; ++i) {
		scop->arrays[i] = array_anonymize(scop->arrays[i]);
		if (!scop->arrays[i])
			return pet_scop_free(scop);
	}

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = stmt_anonymize(scop->stmts[i]);
		if (!scop->stmts[i])
			return pet_scop_free(scop);
	}

	for (i = 0; i < scop->n_implication; ++i) {
		scop->implications[i] =
				implication_anonymize(scop->implications[i]);
		if (!scop->implications[i])
			return pet_scop_free(scop);
	}

	return scop;
}

/* Compute the gist of the iteration domain and all access relations
 * of "stmt" based on the constraints on the parameters specified by "context"
 * and the constraints on the values of nested accesses specified
 * by "value_bounds".
 */
static struct pet_stmt *stmt_gist(struct pet_stmt *stmt,
	__isl_keep isl_set *context, __isl_keep isl_union_map *value_bounds)
{
	int i;
	isl_set *domain;

	if (!stmt)
		return NULL;

	domain = isl_set_copy(stmt->domain);
	if (stmt->n_arg > 0)
		domain = isl_map_domain(isl_set_unwrap(domain));

	domain = isl_set_intersect_params(domain, isl_set_copy(context));

	for (i = 0; i < stmt->n_arg; ++i) {
		stmt->args[i] = pet_expr_gist(stmt->args[i],
							domain, value_bounds);
		if (!stmt->args[i])
			goto error;
	}

	stmt->body = pet_expr_gist(stmt->body, domain, value_bounds);
	if (!stmt->body)
		goto error;

	isl_set_free(domain);

	domain = isl_set_universe(pet_stmt_get_space(stmt));
	domain = isl_set_intersect_params(domain, isl_set_copy(context));
	if (stmt->n_arg > 0)
		domain = pet_value_bounds_apply(domain, stmt->n_arg, stmt->args,
						value_bounds);
	stmt->domain = isl_set_gist(stmt->domain, domain);
	if (!stmt->domain)
		return pet_stmt_free(stmt);

	return stmt;
error:
	isl_set_free(domain);
	return pet_stmt_free(stmt);
}

/* Compute the gist of the extent of the array
 * based on the constraints on the parameters specified by "context".
 */
static struct pet_array *array_gist(struct pet_array *array,
	__isl_keep isl_set *context)
{
	if (!array)
		return NULL;

	array->extent = isl_set_gist_params(array->extent,
						isl_set_copy(context));
	if (!array->extent)
		return pet_array_free(array);

	return array;
}

/* Compute the gist of all sets and relations in "scop"
 * based on the constraints on the parameters specified by "scop->context"
 * and the constraints on the values of nested accesses specified
 * by "value_bounds".
 */
struct pet_scop *pet_scop_gist(struct pet_scop *scop,
	__isl_keep isl_union_map *value_bounds)
{
	int i;

	if (!scop)
		return NULL;

	scop->context = isl_set_coalesce(scop->context);
	if (!scop->context)
		return pet_scop_free(scop);

	for (i = 0; i < scop->n_array; ++i) {
		scop->arrays[i] = array_gist(scop->arrays[i], scop->context);
		if (!scop->arrays[i])
			return pet_scop_free(scop);
	}

	for (i = 0; i < scop->n_stmt; ++i) {
		scop->stmts[i] = stmt_gist(scop->stmts[i], scop->context,
					    value_bounds);
		if (!scop->stmts[i])
			return pet_scop_free(scop);
	}

	return scop;
}

/* Intersect the context of "scop" with "context".
 * To ensure that we don't introduce any unnamed parameters in
 * the context of "scop", we first remove the unnamed parameters
 * from "context".
 */
struct pet_scop *pet_scop_restrict_context(struct pet_scop *scop,
	__isl_take isl_set *context)
{
	if (!scop)
		goto error;

	context = pet_nested_remove_from_set(context);
	scop->context = isl_set_intersect(scop->context, context);
	if (!scop->context)
		return pet_scop_free(scop);

	return scop;
error:
	isl_set_free(context);
	return pet_scop_free(scop);
}

/* Drop the current context of "scop".  That is, replace the context
 * by a universal set.
 */
struct pet_scop *pet_scop_reset_context(struct pet_scop *scop)
{
	isl_space *space;

	if (!scop)
		return NULL;

	space = isl_set_get_space(scop->context);
	isl_set_free(scop->context);
	scop->context = isl_set_universe(space);
	if (!scop->context)
		return pet_scop_free(scop);

	return scop;
}

/* Append "array" to the arrays of "scop".
 */
struct pet_scop *pet_scop_add_array(struct pet_scop *scop,
	struct pet_array *array)
{
	isl_ctx *ctx;
	struct pet_array **arrays;

	if (!array || !scop)
		goto error;

	ctx = isl_set_get_ctx(scop->context);
	arrays = isl_realloc_array(ctx, scop->arrays, struct pet_array *,
				    scop->n_array + 1);
	if (!arrays)
		goto error;
	scop->arrays = arrays;
	scop->arrays[scop->n_array] = array;
	scop->n_array++;

	return scop;
error:
	pet_array_free(array);
	return pet_scop_free(scop);
}

/* Create an index expression for an access to a virtual array
 * representing the result of a condition.
 * Unlike other accessed data, the id of the array is NULL as
 * there is no ValueDecl in the program corresponding to the virtual
 * array.
 * The index expression is created as an identity mapping on "space".
 * That is, the dimension of the array is the same as that of "space".
 */
__isl_give isl_multi_pw_aff *pet_create_test_index(__isl_take isl_space *space,
	int test_nr)
{
	isl_id *id;
	char name[50];

	snprintf(name, sizeof(name), "__pet_test_%d", test_nr);
	id = isl_id_alloc(isl_space_get_ctx(space), name, NULL);
	space = isl_space_map_from_set(space);
	space = isl_space_set_tuple_id(space, isl_dim_out, id);
	return isl_multi_pw_aff_identity(space);
}

/* Add an array with the given extent to the list
 * of arrays in "scop" and return the extended pet_scop.
 * Specifically, the extent is determined by the image of "domain"
 * under "index".
 * "int_size" is the number of bytes needed to represent values of type "int".
 * The array is marked as attaining values 0 and 1 only and
 * as each element being assigned at most once.
 */
struct pet_scop *pet_scop_add_boolean_array(struct pet_scop *scop,
	__isl_take isl_set *domain, __isl_take isl_multi_pw_aff *index,
	int int_size)
{
	isl_ctx *ctx;
	isl_space *space;
	struct pet_array *array;
	isl_map *access;

	if (!scop || !domain || !index)
		goto error;

	ctx = isl_multi_pw_aff_get_ctx(index);
	array = isl_calloc_type(ctx, struct pet_array);
	if (!array)
		goto error;

	access = isl_map_from_multi_pw_aff(index);
	access = isl_map_intersect_domain(access, domain);
	array->extent = isl_map_range(access);
	space = isl_space_params_alloc(ctx, 0);
	array->context = isl_set_universe(space);
	space = isl_space_set_alloc(ctx, 0, 1);
	array->value_bounds = isl_set_universe(space);
	array->value_bounds = isl_set_lower_bound_si(array->value_bounds,
						isl_dim_set, 0, 0);
	array->value_bounds = isl_set_upper_bound_si(array->value_bounds,
						isl_dim_set, 0, 1);
	array->element_type = strdup("int");
	array->element_size = int_size;
	array->uniquely_defined = 1;

	if (!array->extent || !array->context)
		array = pet_array_free(array);

	scop = pet_scop_add_array(scop, array);

	return scop;
error:
	isl_set_free(domain);
	isl_multi_pw_aff_free(index);
	return pet_scop_free(scop);
}

/* Create and return an implication on filter values equal to "satisfied"
 * with extension "map".
 */
static struct pet_implication *new_implication(__isl_take isl_map *map,
	int satisfied)
{
	isl_ctx *ctx;
	struct pet_implication *implication;

	if (!map)
		return NULL;
	ctx = isl_map_get_ctx(map);
	implication = isl_alloc_type(ctx, struct pet_implication);
	if (!implication)
		goto error;

	implication->extension = map;
	implication->satisfied = satisfied;

	return implication;
error:
	isl_map_free(map);
	return NULL;
}

/* Add an implication on filter values equal to "satisfied"
 * with extension "map" to "scop".
 */
struct pet_scop *pet_scop_add_implication(struct pet_scop *scop,
	__isl_take isl_map *map, int satisfied)
{
	isl_ctx *ctx;
	struct pet_implication *implication;
	struct pet_implication **implications;

	implication = new_implication(map, satisfied);
	if (!scop || !implication)
		goto error;

	ctx = isl_set_get_ctx(scop->context);
	implications = isl_realloc_array(ctx, scop->implications,
					    struct pet_implication *,
					    scop->n_implication + 1);
	if (!implications)
		goto error;
	scop->implications = implications;
	scop->implications[scop->n_implication] = implication;
	scop->n_implication++;

	return scop;
error:
	pet_implication_free(implication);
	return pet_scop_free(scop);
}

/* Given an access expression, check if it is data dependent.
 * If so, set *found and abort the search.
 */
static int is_data_dependent(__isl_keep pet_expr *expr, void *user)
{
	int *found = user;

	if (pet_expr_get_n_arg(expr) > 0) {
		*found = 1;
		return -1;
	}

	return 0;
}

/* Does "scop" contain any data dependent accesses?
 *
 * Check the body of each statement for such accesses.
 */
int pet_scop_has_data_dependent_accesses(struct pet_scop *scop)
{
	int i;
	int found = 0;

	if (!scop)
		return -1;

	for (i = 0; i < scop->n_stmt; ++i) {
		int r = pet_expr_foreach_access_expr(scop->stmts[i]->body,
					&is_data_dependent, &found);
		if (r < 0 && !found)
			return -1;
		if (found)
			return found;
	}

	return found;
}

/* Does "scop" contain and data dependent conditions?
 */
int pet_scop_has_data_dependent_conditions(struct pet_scop *scop)
{
	int i;

	if (!scop)
		return -1;

	for (i = 0; i < scop->n_stmt; ++i)
		if (scop->stmts[i]->n_arg > 0)
			return 1;

	return 0;
}

/* Keep track of the "input" file inside the (extended) "scop".
 */
struct pet_scop *pet_scop_set_input_file(struct pet_scop *scop, FILE *input)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;

	if (!scop)
		return NULL;

	ext->input = input;

	return scop;
}

/* Print the original code corresponding to "scop" to printer "p".
 *
 * pet_scop_print_original can only be called from
 * a pet_transform_C_source callback.  This means that the input
 * file is stored in the extended scop and that the printer prints
 * to a file.
 */
__isl_give isl_printer *pet_scop_print_original(struct pet_scop *scop,
	__isl_take isl_printer *p)
{
	struct pet_scop_ext *ext = (struct pet_scop_ext *) scop;
	FILE *output;
	unsigned start, end;

	if (!scop || !p)
		return isl_printer_free(p);

	if (!ext->input)
		isl_die(isl_printer_get_ctx(p), isl_error_invalid,
			"no input file stored in scop",
			return isl_printer_free(p));

	output = isl_printer_get_file(p);
	if (!output)
		return isl_printer_free(p);

	start = pet_loc_get_start(scop->loc);
	end = pet_loc_get_end(scop->loc);
	if (copy(ext->input, output, start, end) < 0)
		return isl_printer_free(p);

	return p;
}
