import ida_auto
import ida_funcs
import ida_hexrays
import ida_ida
import ida_idaapi
import ida_ua
import idautils
import idc


OUTPUT_PATH = r"E:\G-Force\g_force\tools\coop_v2\ida_trace_input.txt"
output = open(OUTPUT_PATH, "w", encoding="utf-8")


def emit(text=""):
    output.write(str(text) + "\n")
    output.flush()


def dump_function(ea):
    function = ida_funcs.get_func(ea)
    if not function:
        emit("NO_FUNCTION 0x%08X" % ea)
        return
    emit("\n===== FUNCTION 0x%08X %s =====" %
         (function.start_ea, idc.get_func_name(function.start_ea)))
    try:
        emit(str(ida_hexrays.decompile(function.start_ea)))
    except Exception as error:
        emit("DECOMPILE_FAILED %s" % error)
        for item in idautils.FuncItems(function.start_ea):
            emit("%08X  %s" % (item, idc.generate_disasm_line(item, 0)))


try:
    emit("TRACE_START")
    targets = {
        0x004B6F00,
        0x004B7050,
        0x00488A70,
        0x00488B00,
        0x00488CE0,
        0x00488DC0,
        0x005B8C20,
        0x005BB1D0,
        0x005BBBB0,
        0x005BEA00,
        0x005BEAC0,
		0x005BEB60,
		0x005BF6D0,
		0x005C8E60,
        0x005BFBE0,
    }
    for target in sorted(targets):
        ida_funcs.add_func(target)
        dump_function(target)
except Exception:
    import traceback
    emit(traceback.format_exc())
finally:
    output.close()
    ida_idaapi.qexit(0)
