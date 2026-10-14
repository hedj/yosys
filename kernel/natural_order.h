/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef NATURAL_ORDER_H
#define NATURAL_ORDER_H

#include "kernel/rtlil.h"

YOSYS_NAMESPACE_BEGIN

// Order names that must be ordered reproducibly. IdString's operator<
// compares positions in the global name table, which depend on every name
// the process has interned before, including names from designs that were
// later discarded. A plain string compare is reproducible but puts
// `$foo$10000` before `$foo$9999`, so it still depends on how far autoidx had
// advanced. Comparing runs of digits by value orders auto-generated names by
// creation, independent of both.
inline bool lt_natural(const std::string &a, const std::string &b)
{
	size_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
			size_t i0 = i, j0 = j;
			while (i0 < a.size() && a[i0] == '0') i0++;
			while (j0 < b.size() && b[j0] == '0') j0++;
			size_t i1 = i0, j1 = j0;
			while (i1 < a.size() && isdigit((unsigned char)a[i1])) i1++;
			while (j1 < b.size() && isdigit((unsigned char)b[j1])) j1++;
			if (i1 - i0 != j1 - j0)
				return i1 - i0 < j1 - j0;
			int c = a.compare(i0, i1 - i0, b, j0, j1 - j0);
			if (c != 0)
				return c < 0;
			if (i1 - i != j1 - j)
				return i1 - i < j1 - j;
			i = i1, j = j1;
			continue;
		}
		if (a[i] != b[j])
			return a[i] < b[j];
		i++, j++;
	}
	return a.size() - i < b.size() - j;
}

inline bool lt_natural(RTLIL::IdString a, RTLIL::IdString b)
{
	return a != b && lt_natural(a.str(), b.str());
}

template<typename T> struct compare_ptr_by_natural_name {
	bool operator()(const T *a, const T *b) const {
		return (a == nullptr || b == nullptr) ? (a < b) : lt_natural(a->name, b->name);
	}
};

YOSYS_NAMESPACE_END

#endif
