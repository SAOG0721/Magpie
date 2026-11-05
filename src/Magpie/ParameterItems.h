#pragma once
#include <cstdint>

namespace Magpie {
// Both lists have the same semantic order. Retain surviving items rather than
// resetting ItemsSource: a focused ComboBox may still own an open native popup.
template<class Vector, class Items>
void SynchronizeParameterItems(const Vector& current, const Items& desired) {
	uint32_t index = 0;
	for (const auto& item : desired) {
		uint32_t found = index;
		while (found < current.Size() && current.GetAt(found) != item) ++found;
		if (found == current.Size()) current.InsertAt(index, item);
		else while (found > index) { current.RemoveAt(index); --found; }
		++index;
	}
	while (current.Size() > index) current.RemoveAtEnd();
}
}
