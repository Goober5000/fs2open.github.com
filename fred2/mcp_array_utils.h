#pragma once

// General-purpose array/vector manipulation utilities for MCP tools.
// These are type-agnostic operations (insert, remove, move) that can be
// reused across different mission entity types (messages, events, etc.).

#include <algorithm>
#include <utility>
#include <vector>

#include "globalincs/pstypes.h"

// ---------------------------------------------------------------------------
// Raw array overloads (array + count + max_size)
// ---------------------------------------------------------------------------

// Open a slot at `index` by shifting elements [index, count) right by one.
// Increments count.  Returns false if the array is already full.
template <typename T>
bool array_insert_slot(T *arr, int &count, int max_size, int index)
{
	Assertion(index >= 0 && index <= count, "array_insert_slot: index %d out of range [0, %d]", index, count);
	if (count >= max_size)
		return false;
	for (int i = count; i > index; i--)
		arr[i] = std::move(arr[i - 1]);
	count++;
	return true;
}

// Close a slot at `index` by shifting elements (index, count) left by one.
// Decrements count.
template <typename T>
void array_remove_slot(T *arr, int &count, int index)
{
	Assertion(index >= 0 && index < count, "array_remove_slot: index %d out of range [0, %d)", index, count);
	for (int i = index; i < count - 1; i++)
		arr[i] = std::move(arr[i + 1]);
	count--;
}

// Move element at `from` to `to`, shifting intermediate elements.
template <typename T>
void array_move_element(T *arr, int count, int from, int to)
{
	Assertion(from >= 0 && from < count, "array_move_element: from %d out of range [0, %d)", from, count);
	Assertion(to >= 0 && to < count, "array_move_element: to %d out of range [0, %d)", to, count);
	if (from == to)
		return;
	T temp = std::move(arr[from]);
	if (from < to) {
		for (int i = from; i < to; i++)
			arr[i] = std::move(arr[i + 1]);
	} else {
		for (int i = from; i > to; i--)
			arr[i] = std::move(arr[i - 1]);
	}
	arr[to] = std::move(temp);
}

// ---------------------------------------------------------------------------
// Vector overloads
// ---------------------------------------------------------------------------

// Open a slot at `index` by shifting elements [index, count) right by one.
// Grows the vector if needed.  Increments count.
template <typename T>
void array_insert_slot(std::vector<T> &vec, int &count, int index)
{
	Assertion(index >= 0 && index <= count, "array_insert_slot: index %d out of range [0, %d]", index, count);
	if (count >= (int)vec.size())
		vec.resize(count + 1);
	for (int i = count; i > index; i--)
		vec[i] = std::move(vec[i - 1]);
	count++;
}

// Close a slot at `index` by shifting elements (index, count) left by one.
// Decrements count.  Does not shrink the vector.
template <typename T>
void array_remove_slot(std::vector<T> &vec, int &count, int index)
{
	Assertion(index >= 0 && index < count, "array_remove_slot: index %d out of range [0, %d)", index, count);
	for (int i = index; i < count - 1; i++)
		vec[i] = std::move(vec[i + 1]);
	count--;
}

// Move element at `from` to `to`, shifting intermediate elements.
template <typename T>
void array_move_element(std::vector<T> &vec, int from, int to)
{
	Assertion(from >= 0 && from < (int)vec.size(), "array_move_element: from %d out of range [0, %d)", from, (int)vec.size());
	Assertion(to >= 0 && to < (int)vec.size(), "array_move_element: to %d out of range [0, %d)", to, (int)vec.size());
	if (from == to)
		return;
	T temp = std::move(vec[from]);
	if (from < to) {
		for (int i = from; i < to; i++)
			vec[i] = std::move(vec[i + 1]);
	} else {
		for (int i = from; i > to; i--)
			vec[i] = std::move(vec[i - 1]);
	}
	vec[to] = std::move(temp);
}

// Linear search in a sentinel-terminated (t->name == nullptr) named-info table.
template<typename T>
const T *find_named_info(const T *table, const char *name)
{
	for (const T *t = table; t->name; t++)
		if (stricmp(name, t->name) == 0)
			return t;
	return nullptr;
}
