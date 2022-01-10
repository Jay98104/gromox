// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <gromox/tarray_set.hpp>
#include <cstdlib>
#include <cstring>

TARRAY_SET* tarray_set_init()
{
	auto pset = static_cast<TARRAY_SET *>(malloc(sizeof(TARRAY_SET)));
	if (NULL == pset) {
		return NULL;
	}
	pset->count = 0;
	auto count = strange_roundup(pset->count, SR_GROW_TPROPVAL_ARRAY);
	pset->pparray = static_cast<TPROPVAL_ARRAY **>(malloc(sizeof(TPROPVAL_ARRAY *) * count));
	if (NULL == pset->pparray) {
		free(pset);
		return NULL;
	}
	return pset;
}

void tarray_set_free(TARRAY_SET *pset)
{
	for (size_t i = 0; i < pset->count; ++i)
		if (NULL != pset->pparray[i]) {
			tpropval_array_free(pset->pparray[i]);
		}
	free(pset->pparray);
	free(pset);
}

void tarray_set::erase(uint32_t index)
{
	auto pset = this;
	TPROPVAL_ARRAY *parray;
	
	if (index >= pset->count) {
		return;
	}
	parray = pset->pparray[index];
	pset->count --;
	if (index != pset->count) {
		memmove(pset->pparray + index, pset->pparray +
			index + 1, sizeof(void*)*(pset->count - index));
	}
	tpropval_array_free(parray);
}

bool tarray_set::append_move(TPROPVAL_ARRAY *pproplist)
{
	auto pset = this;
	TPROPVAL_ARRAY **pparray;
	
	if (pset->count >= 0xFF00) {
		return false;
	}
	auto count = strange_roundup(pset->count, SR_GROW_TPROPVAL_ARRAY);
	if (pset->count + 1 >= count) {
		count += SR_GROW_TPROPVAL_ARRAY;
		pparray = static_cast<TPROPVAL_ARRAY **>(realloc(pset->pparray, count * sizeof(TPROPVAL_ARRAY *)));
		if (NULL == pparray) {
			return false;
		}
		pset->pparray = pparray;
	}
	pset->pparray[pset->count] = pproplist;
	pset->count ++;
	return true;
}

tarray_set *tarray_set::dup() const
{
	auto pset = this;
	TARRAY_SET *pset1;
	
	pset1 = tarray_set_init();
	if (NULL == pset1) {
		return NULL;
	}
	for (size_t i = 0; i < pset->count; ++i) {
		auto pproplist = pset->pparray[i]->dup();
		if (NULL == pproplist) {
			tarray_set_free(pset1);
			return NULL;
		}
		if (!pset1->append_move(pproplist)) {
			tpropval_array_free(pproplist);
			tarray_set_free(pset1);
			return NULL;
		}
	}
	return pset1;
}
