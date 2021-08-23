/*
* Copyright 2018-2021 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_distinct.h"
#include "op_project.h"
#include "op_aggregate.h"
#include "xxhash.h"
#include "../../../../util/arr.h"
#include "../../../execution_plan_build/execution_plan_modify.h"

/* Forward declarations. */
static Record DistinctConsume(RT_OpBase *opBase);
static RT_OpBase *DistinctClone(const RT_ExecutionPlan *plan, const RT_OpBase *opBase);
static void DistinctFree(RT_OpBase *opBase);

// compute hash on distinct values
// values that are required to be distinct are located at 'offset'
// positions within the record
static unsigned long long _compute_hash(RT_OpDistinct *op, Record r) {
	// initialize the hash state
	XXH64_state_t state;
	XXH_errorcode res = XXH64_reset(&state, 0);
	ASSERT(res != XXH_ERROR);

	for(uint i = 0; i < op->offset_count; i++) {
		// retrieve the entry at 'idx' as an SIValue
		uint idx = op->offsets[i];
		SIValue v = Record_Get(r, idx);
		// update the hash state with the current value.
		SIValue_HashUpdate(v, &state);
	}

	// finalize the hash
	unsigned long long const hash = XXH64_digest(&state);
	return hash;
}

// compute record offset to distinct values
static void _updateOffsets(RT_OpDistinct *op, Record r) {
	ASSERT(op->aliases != NULL);
	ASSERT(op->offsets != NULL);

	for(uint i = 0; i < op->offset_count; i++) {
		uint offset = Record_GetEntryIdx(r, op->aliases[i]);
		ASSERT(offset != INVALID_INDEX);
		op->offsets[i] = offset;
	}
}

RT_OpBase *RT_NewDistinctOp(const RT_ExecutionPlan *plan, const char **aliases, uint alias_count) {
	ASSERT(aliases != NULL);
	ASSERT(alias_count > 0);

	RT_OpDistinct *op = rm_malloc(sizeof(RT_OpDistinct));

	op->found           =  raxNew();
	op->mapping         =  NULL;
	op->aliases         =  rm_malloc(alias_count * sizeof(const char *));
	op->offset_count    =  alias_count;
	op->offsets         =  rm_calloc(op->offset_count, sizeof(uint));

	// Copy aliases into heap array managed by this op
	memcpy(op->aliases, aliases, alias_count * sizeof(const char *));

	RT_OpBase_Init((RT_OpBase *)op, OPType_DISTINCT, NULL, DistinctConsume,
				NULL, DistinctClone, DistinctFree, false, plan);

	return (RT_OpBase *)op;
}

static Record DistinctConsume(RT_OpBase *opBase) {
	RT_OpDistinct *op = (RT_OpDistinct *)opBase;
	RT_OpBase *child = op->op.children[0];

	while(true) {
		Record r = RT_OpBase_Consume(child);
		if(!r) return NULL;

		// update offsets if record mapping changed
		// it is possible for the record's mapping to be changed throughtout
		// the execution as this distinct operation might recieve records from
		// different sub execution plans, such as in the case of UNION
		// in which case the distinct values might be located at different offsets
		// within the record and we should adjust accordingly
		rax *record_mapping = Record_GetMappings(r);
		if(record_mapping != op->mapping) {
			// record mapping changed, update offsets
			_updateOffsets(op, r);
			// update operation mapping to records mapping
			op->mapping = record_mapping;
		}

		unsigned long long const hash = _compute_hash(op, r);
		int is_new = raxInsert(op->found, (unsigned char *) &hash, sizeof(hash), NULL, NULL);
		if(is_new) return r;
		RT_OpBase_DeleteRecord(r);
	}
}

static inline RT_OpBase *DistinctClone(const RT_ExecutionPlan *plan, const RT_OpBase *opBase) {
	ASSERT(opBase->type == OPType_DISTINCT);
	RT_OpDistinct *op = (RT_OpDistinct *)opBase;
	return RT_NewDistinctOp(plan, op->aliases, op->offset_count);
}

static void DistinctFree(RT_OpBase *ctx) {
	RT_OpDistinct *op = (RT_OpDistinct *)ctx;
	if(op->found) {
		raxFree(op->found);
		op->found = NULL;
	}

	if(op->aliases) {
		rm_free(op->aliases);
		op->aliases = NULL;
	}

	if(op->offsets) {
		rm_free(op->offsets);
		op->offsets = NULL;
	}
}