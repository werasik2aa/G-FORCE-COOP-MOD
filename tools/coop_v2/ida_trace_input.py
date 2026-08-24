import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_ida
import ida_idaapi
import ida_ua
import idautils
import idc

try:
    import ida_pro
except ImportError:
    ida_pro = None

try:
    import ida_kernwin
except ImportError:
    ida_kernwin = None


OUTPUT_PATH = r"E:\G-Force\g_force\tools\coop_v2\ida_trace_input.txt"
output = open(OUTPUT_PATH, "w", encoding="utf-8")


def emit(text=""):
    output.write(str(text) + "\n")
    output.flush()


def disasm_fallback(start_ea):
    for item in idautils.FuncItems(start_ea):
        emit("%08X  %s" % (item, idc.generate_disasm_line(item, 0)))


def dump_function(ea, note=""):
    ida_funcs.add_func(ea)
    function = ida_funcs.get_func(ea)
    if not function:
        emit("\nNO_FUNCTION 0x%08X %s" % (ea, note))
        return
    emit("\n===== FUNCTION 0x%08X %s %s =====" %
         (function.start_ea, idc.get_func_name(function.start_ea), note))
    try:
        decompiled = ida_hexrays.decompile(function.start_ea)
    except Exception as error:
        emit("DECOMPILE_EXCEPTION %s" % error)
        disasm_fallback(function.start_ea)
        return
    # decompile() returns None (no exception) when Hex-Rays silently declines;
    # the old script printed the literal "None" and lost the function.  Fall
    # back to a linear disassembly so no target is ever blank.
    if decompiled is None:
        emit("DECOMPILE_RETURNED_NONE - linear disassembly follows")
        disasm_fallback(function.start_ea)
        return
    emit(str(decompiled))


def read_ptr(ea):
    try:
        return ida_bytes.get_dword(ea)
    except Exception:
        return 0


def dump_vtable_slot(vtable_ea, slot_offset, label):
    # Mode vtable layout (from SelectMode 0x4B7050 and the mode dispatcher
    # 0x4B6F00): +0x08 Enter, +0x0C Update, +0x14 Exit.  Controller vtable Update
    # is +0x08 (the hooked GPig slot 0x70C8A4 = XController_GPig 0x70C89C + 8).
    slot_ea = vtable_ea + slot_offset
    target = read_ptr(slot_ea)
    emit("\n--- %s: [0x%08X + 0x%02X] = 0x%08X ---" %
         (label, vtable_ea, slot_offset, target))
    if target:
        dump_function(target, "(%s)" % label)


try:
    emit("TRACE_START")

    # 1) Functions that resolve the Darwin -> Mooch -> Darwin path directly.
    #    0x5BBC80 is the GPig_Default Mooch switch: it reads the pressed edge of
    #    action 0x1000000E (0x488CE0 at 0x5BBCAC) and calls [vtable+0x20] with the
    #    Mooch-switch mode id 0x61000065 at 0x5BBD94.  0x488CE0 is the pressed-edge
    #    query itself and previously decompiled to None, so it now falls back to
    #    disassembly.
    direct_targets = [
        (0x005BBC80, "GPig_Default Mooch switch (edge 0x1000000E -> 0x61000065)"),
        (0x00488CE0, "pressed-edge query (rising edge)"),
        (0x00488C00, "released-edge query (falling edge)"),
        (0x005BBBB0, "GPig_Default aim/turn helper (context around switch)"),
        (0x004B7050, "SelectMode"),
        (0x004B6F00, "mode dispatcher (calls current mode Update +0x0C)"),
        (0x005BFBE0, "XController base Update (this[12]=0; dispatch)"),
    ]
    for ea, note in direct_targets:
        dump_function(ea, note)

    # 2) The mode/controller Update bodies, resolved straight from the vtables so
    #    no address is guessed.  These four are the missing disasm that gate the
    #    Mooch hand-off and the fly's own tick.
    #      XControllerMode_GPig_Mooch  vt=0x0071839C  Update = +0x0C
    #      XControllerMode_Fly_Active  vt=0x007180A4  Update = +0x0C  (0x61000034)
    #      XControllerMode_Fly_Idle    vt=0x00718014  Update = +0x0C  (0x61000033)
    #      XController_Fly             vt=0x007180EC  Update = +0x08
    #    The controller Update is where we learn whether the fly shares the base
    #    0x5BFBE0 (so hooking 0x007180F4 would intercept it) or has its own body.
    dump_vtable_slot(0x0071839C, 0x0C, "GPig_Mooch::Update")
    dump_vtable_slot(0x0071839C, 0x08, "GPig_Mooch::Enter")
    dump_vtable_slot(0x007180A4, 0x0C, "Fly_Active::Update")
    dump_vtable_slot(0x007180A4, 0x08, "Fly_Active::Enter")
    dump_vtable_slot(0x00718014, 0x0C, "Fly_Idle::Update")
    dump_vtable_slot(0x007180EC, 0x08, "XController_Fly::Update")

    # 3) Also dump the raw controller/mode Update slots so the hooked-vs-unhooked
    #    comparison is explicit in the output.
    emit("\n--- vtable Update slots (controller Update = +0x08) ---")
    emit("XController_GPig::Update  [0x0070C8A4] = 0x%08X  (hooked by the DLL)" %
         read_ptr(0x0070C8A4))
    emit("XController_Fly::Update   [0x007180F4] = 0x%08X  (NOT hooked)" %
         read_ptr(0x007180F4))

    # 4) Fire/aim context kept from the previous run for continuity.
    context_targets = [
        (0x00488A70, "is-down query"),
        (0x00488B00, "aim-hold query"),
        (0x00488DC0, "hold+threshold query"),
        (0x005BEA00, "GPig_Default::Update (fire dispatch)"),
        (0x005B8C20, "fire context"),
        (0x005BB1D0, "aim/weapon state machine"),
    ]
    for ea, note in context_targets:
        dump_function(ea, note)

except Exception:
    import traceback
    emit(traceback.format_exc())
finally:
    output.close()
    # Only quit the process in headless/batch runs (idat -A -S...).  When the
    # script is run from an open GUI (File -> Script file) do NOT qexit, or it
    # would tear down the user's live IDA session.  ida_idaapi.qexit was removed
    # in IDA 9.x; ida_pro.qexit is the stable name.
    in_gui = bool(ida_kernwin and hasattr(ida_kernwin, "is_idaq")
                  and ida_kernwin.is_idaq())
    if not in_gui:
        if ida_pro is not None and hasattr(ida_pro, "qexit"):
            ida_pro.qexit(0)
        elif hasattr(idc, "qexit"):
            idc.qexit(0)
        else:
            ida_idaapi.qexit(0)
