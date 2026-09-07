"""Batch-only IDA trace for the observed G-Force object-event routes.

Run against the existing GForce.exe.i64 with `-c` so the database is not saved.
It prints only the target functions, their direct xrefs, and Hex-Rays output when
available.  The addresses are observations from live co-op logs, not names guessed
from event values.
"""

import ida_funcs
import ida_hexrays
import ida_idaapi
import ida_name
import ida_xref
import idautils


TARGETS = (
    (0x0041E890, "object relay"),
    (0x0044BF70, "observed door proximity caller"),
    (0x0046D6F0, "object forwarder"),
    (0x0042AE70, "XTrigger_CO_Door virtual implementation range"),
    (0x0042DA20, "XTrigger_MO_Blender virtual implementation range"),
)


def function_name(ea):
    return ida_name.get_name(ea) or "sub_%08X" % ea


def print_xrefs(ea, direction):
    if direction == "to":
        refs = idautils.XrefsTo(ea, 0)
    else:
        refs = idautils.XrefsFrom(ea, 0)
    for ref in refs:
        print("  xref-%s %08X -> %08X type=%d" %
              (direction, ref.frm, ref.to, ref.type))


def print_decompilation(ea):
    try:
        cfunc = ida_hexrays.decompile(ea)
    except Exception as error:
        print("  decompile-error: %s" % error)
        return
    if cfunc is None:
        print("  decompile-unavailable")
        return
    print(str(cfunc))


def main():
    print("[ida-object-event-trace] begin")
    for ea, label in TARGETS:
        func = ida_funcs.get_func(ea)
        if func is None:
            print("[target] %s %08X function=missing" % (label, ea))
            continue
        start = func.start_ea
        print("[target] %s requested=%08X function=%08X..%08X name=%s" %
              (label, ea, start, func.end_ea, function_name(start)))
        print_xrefs(start, "to")
        print_xrefs(start, "from")
        print_decompilation(start)
    print("[ida-object-event-trace] end")


if __name__ == "__main__":
    main()
    ida_idaapi.qexit(0)
