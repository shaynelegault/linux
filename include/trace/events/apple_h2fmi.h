/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM apple_h2fmi

#if !defined(_TRACE_APPLE_H2FMI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_APPLE_H2FMI_H

#include <linux/tracepoint.h>

/* kind: 0 request, 1 staged opcode, 2 status reply, 3 return, 4 I/O launch. */
TRACE_EVENT(apple_h2fmi_transaction,
	TP_PROTO(const char *device, u64 seq, u64 request, u8 bus, u8 kind,
		 u16 opcode, u32 row, int value),
	TP_ARGS(device, seq, request, bus, kind, opcode, row, value),
	TP_STRUCT__entry(
		__string(device, device)
		__field(u64, seq)
		__field(u64, request)
		__field(u32, row)
		__field(int, value)
		__field(u16, opcode)
		__field(u8, bus)
		__field(u8, kind)
	),
	TP_fast_assign(
		__assign_str(device);
		__entry->seq = seq;
		__entry->request = request;
		__entry->bus = bus;
		__entry->kind = kind;
		__entry->opcode = opcode;
		__entry->row = row;
		__entry->value = value;
	),
	TP_printk("dev=%s seq=%llu req=%llu bus=%u kind=%u op=%04x row=%08x value=%d",
		  __get_str(device), __entry->seq, __entry->request,
		  __entry->bus, __entry->kind, __entry->opcode, __entry->row,
		  __entry->value)
);

#endif /* _TRACE_APPLE_H2FMI_H */

#include <trace/define_trace.h>
