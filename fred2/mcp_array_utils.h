#pragma once

// General-purpose array/vector manipulation utilities for MCP tools.
// These are type-agnostic operations (insert, remove, move) that can be
// reused across different mission entity types (messages, events, etc.).

#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Raw array overloads (array + count + max_size)
// ---------------------------------------------------------------------------

// Open a slot at `index` by shifting elements [index, count) right by one.
// Increments count.  Returns false if the array is already full.
template <typename T>
bool array_insert_slot(T *arr, int &count, int max_size, int index)
{
	if (count >= max_size)
		return false;
	for (int i = count; i > index; i--)
		arr[i] = arr[i - 1];
	count++;
	return true;
}

// Close a slot at `index` by shifting elements (index, count) left by one.
// Decrements count.
template <typename T>
void array_remove_slot(T *arr, int &count, int index)
{
	for (int i = index; i < count - 1; i++)
		arr[i] = arr[i + 1];
	count--;
}

// Move element at `from` to `to`, shifting intermediate elements.
template <typename T>
void array_move_element(T *arr, int from, int to)
{
	if (from == to)
		return;
	T temp = arr[from];
	if (from < to) {
		for (int i = from; i < to; i++)
			arr[i] = arr[i + 1];
	} else {
		for (int i = from; i > to; i--)
			arr[i] = arr[i - 1];
	}
	arr[to] = temp;
}

// ---------------------------------------------------------------------------
// Vector overloads
// ---------------------------------------------------------------------------

// Open a slot at `index` by shifting elements [index, count) right by one.
// Grows the vector if needed.  Increments count.
template <typename T>
void array_insert_slot(std::vector<T> &vec, int &count, int index)
{
	if (count >= (int)vec.size())
		vec.resize(count + 1);
	for (int i = count; i > index; i--)
		vec[i] = vec[i - 1];
	count++;
}

// Close a slot at `index` by shifting elements (index, count) left by one.
// Decrements count.  Does not shrink the vector.
template <typename T>
void array_remove_slot(std::vector<T> &vec, int &count, int index)
{
	for (int i = index; i < count - 1; i++)
		vec[i] = vec[i + 1];
	count--;
}

// Move element at `from` to `to`, shifting intermediate elements.
template <typename T>
void array_move_element(std::vector<T> &vec, int from, int to)
{
	if (from == to)
		return;
	T temp = vec[from];
	if (from < to) {
		for (int i = from; i < to; i++)
			vec[i] = vec[i + 1];
	} else {
		for (int i = from; i > to; i--)
			vec[i] = vec[i - 1];
	}
	vec[to] = temp;
}
