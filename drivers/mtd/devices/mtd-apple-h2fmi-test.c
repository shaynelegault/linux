// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "mtd-apple-h2fmi.h"

static const u32 ppn_16gb[] = {
	2, 4, 1064, 11, 256, 128, 8, 1, 2, 16448,
};

static const u32 ppn_64gb[] = {
	8, 3, 2088, 12, 128, 64, 7, 1, 2, 16448,
};

static void h2fmi_test_params(u8 *params, const u32 *fields)
{
	int i;

	memset(params, 0, H2FMI_PARAMS_SIZE);
	memcpy(params, "PPN Device Info", sizeof("PPN Device Info") - 1);
	for (i = 0; i < ARRAY_SIZE(ppn_64gb); i++)
		put_unaligned_le32(fields[i], params + 0x10 + i * sizeof(u32));
}

static void h2fmi_geometry_test(struct kunit *test)
{
	u8 params[H2FMI_PARAMS_SIZE];
	struct h2fmi_geometry g;

	h2fmi_test_params(params, ppn_16gb);
	KUNIT_ASSERT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
	KUNIT_EXPECT_EQ(test, g.caus, 2U);
	KUNIT_EXPECT_EQ(test, g.slc_pages, 128U);
	KUNIT_EXPECT_EQ(test, (u64)g.blocks * g.pages * H2FMI_PAGE_DATA, 4462739456ULL);
	h2fmi_test_params(params, ppn_64gb);
	KUNIT_ASSERT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
	KUNIT_EXPECT_EQ(test, g.caus, 8U);
	KUNIT_EXPECT_EQ(test, g.slc_pages, 64U);
	KUNIT_EXPECT_EQ(test, (u64)g.blocks * g.pages * H2FMI_PAGE_DATA, 4378853376ULL);
	KUNIT_EXPECT_EQ(test, 2ULL * g.caus * g.blocks * g.pages * H2FMI_PAGE_DATA,
			70061654016ULL);
}

static void h2fmi_invalid_geometry_test(struct kunit *test)
{
	u8 params[H2FMI_PARAMS_SIZE];
	struct h2fmi_geometry g, sentinel;
	int i;

	memset(&sentinel, 0xa5, sizeof(sentinel));
	for (i = 0; i < ARRAY_SIZE(ppn_64gb); i++) {
		h2fmi_test_params(params, ppn_64gb);
		put_unaligned_le32(0, params + 0x10 + i * sizeof(u32));
		g = sentinel;
		KUNIT_EXPECT_LT(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
		KUNIT_EXPECT_MEMEQ(test, &g, &sentinel, sizeof(g));
	}
	h2fmi_test_params(params, ppn_64gb);
	KUNIT_EXPECT_EQ(test, h2fmi_parse_geometry(params, sizeof(params) - 1, &g), -EINVAL);
	KUNIT_EXPECT_EQ(test, h2fmi_parse_geometry(params, 0, &g), -EINVAL);
	params[0] ^= 1;
	KUNIT_EXPECT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), -EINVAL);
}

static void h2fmi_row_test(struct kunit *test)
{
	u8 params[H2FMI_PARAMS_SIZE];
	struct h2fmi_geometry g;
	u32 row, previous = 0;
	unsigned int cau, block, page;

	h2fmi_test_params(params, ppn_64gb);
	KUNIT_ASSERT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
	KUNIT_ASSERT_EQ(test, h2fmi_page_row(&g, 0, 2048, 0, &row), 0);
	KUNIT_EXPECT_EQ(test, row, 0x40000U);
	KUNIT_ASSERT_EQ(test, h2fmi_page_row(&g, 1, 0, 0, &row), 0);
	KUNIT_EXPECT_EQ(test, row, 0x80000U);
	for (cau = 0; cau < g.caus; cau++) {
		for (block = 0; block < g.blocks; block++) {
			for (page = 0; page < g.pages; page++) {
				h2fmi_page_row(&g, cau, block, page, &row);
				if (cau || block || page) {
					if (row <= previous) {
						KUNIT_FAIL(test, "PPN row alias");
						return;
					}
				}
				previous = row;
			}
		}
	}
	KUNIT_EXPECT_EQ(test, previous, 0x3c13ffU);
	row = 0xdeadbeef;
	KUNIT_EXPECT_EQ(test, h2fmi_page_row(&g, 8, 0, 0, &row), -EINVAL);
	KUNIT_EXPECT_EQ(test, h2fmi_page_row(&g, 0, 2088, 0, &row), -EINVAL);
	KUNIT_EXPECT_EQ(test, h2fmi_page_row(&g, 0, 0, 128, &row), -EINVAL);
	KUNIT_EXPECT_EQ(test, row, 0xdeadbeefU);
	h2fmi_test_params(params, ppn_16gb);
	KUNIT_ASSERT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
	KUNIT_ASSERT_EQ(test, h2fmi_page_row(&g, 1, 1063, 255, &row), 0);
	KUNIT_EXPECT_EQ(test, row, 0xc27ffU);
}

static void h2fmi_slc_row_test(struct kunit *test)
{
	u8 params[H2FMI_PARAMS_SIZE];
	struct h2fmi_geometry g;
	u32 row, previous = 0;
	unsigned int cau, block, page;

	h2fmi_test_params(params, ppn_64gb);
	KUNIT_ASSERT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
	KUNIT_ASSERT_EQ(test, h2fmi_slc_row(&g, 0, 2087, 0, &row), 0);
	KUNIT_EXPECT_EQ(test, row, 0x441380U);
	for (cau = 0; cau < g.caus; cau++) {
		for (block = 0; block < g.blocks; block++) {
			for (page = 0; page < g.slc_pages; page++) {
				h2fmi_slc_row(&g, cau, block, page, &row);
				if (row <= previous || !(row & 0x400000)) {
					KUNIT_FAIL(test, "PPN SLC row alias or mode bit lost");
					return;
				}
				previous = row;
			}
		}
	}
	KUNIT_EXPECT_EQ(test, previous, 0x7c13bfU);
	row = 0xdeadbeef;
	KUNIT_EXPECT_EQ(test, h2fmi_slc_row(&g, 8, 0, 0, &row), -EINVAL);
	KUNIT_EXPECT_EQ(test, h2fmi_slc_row(&g, 0, 2088, 0, &row), -EINVAL);
	KUNIT_EXPECT_EQ(test, h2fmi_slc_row(&g, 0, 0, 64, &row), -EINVAL);
	KUNIT_EXPECT_EQ(test, row, 0xdeadbeefU);
	h2fmi_test_params(params, ppn_16gb);
	KUNIT_ASSERT_EQ(test, h2fmi_parse_geometry(params, sizeof(params), &g), 0);
	KUNIT_ASSERT_EQ(test, h2fmi_slc_row(&g, 1, 1063, 127, &row), 0);
	KUNIT_EXPECT_EQ(test, row, 0x8c277fU);
	KUNIT_EXPECT_EQ(test, h2fmi_slc_row(&g, 0, 0, 128, &row), -EINVAL);
}

static void h2fmi_status_test(struct kunit *test)
{
	unsigned int status;

	for (status = 0; status <= 0xff; status++) {
		int result = h2fmi_page_status(status);

		if (status == PPN_ST_GOOD || status == PPN_ST_ERASED)
			KUNIT_EXPECT_EQ(test, result, 0);
		else if (status == PPN_ST_REFRESH)
			KUNIT_EXPECT_EQ(test, result, 1);
		else
			KUNIT_EXPECT_EQ(test, result, -EBADMSG);
	}
}

static void h2fmi_program_status_test(struct kunit *test)
{
	unsigned int status;

	for (status = 0; status <= 0xff; status++) {
		bool ready = status & 0x40;
		bool failed = status & 0x11;

		KUNIT_EXPECT_EQ(test, h2fmi_program_status(status),
				ready && !failed ? 0 : -EIO);
	}
}

static struct kunit_case h2fmi_test_cases[] = {
	KUNIT_CASE(h2fmi_geometry_test),
	KUNIT_CASE(h2fmi_invalid_geometry_test),
	KUNIT_CASE(h2fmi_row_test),
	KUNIT_CASE(h2fmi_status_test),
	KUNIT_CASE(h2fmi_slc_row_test),
	KUNIT_CASE(h2fmi_program_status_test),
	{}
};

static struct kunit_suite h2fmi_test_suite = {
	.name = "apple-h2fmi",
	.test_cases = h2fmi_test_cases,
};

kunit_test_suite(h2fmi_test_suite);

MODULE_LICENSE("GPL");
