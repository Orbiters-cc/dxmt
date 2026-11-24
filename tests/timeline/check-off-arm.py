#!/usr/bin/env python3
"""Without OBELISK_DXMT_QUERY_RESCUE and OBELISK_TIMELINE, the query paths run the function bodies of the commit that was measured:
compares their text, as it is in the working tree, with that commit's. Usage: check-off-arm.py [<commit, 4e3a952>]   (in the checkout)

What may differ is named below, and nothing else: an access specifier, and in the shared base class (d3d11_context_impl.cpp) the
timeline's argument of InvalidateCurrentPass and its name for `promote_flush = true`."""
import re, subprocess, sys

BASE = sys.argv[1] if len(sys.argv) > 1 else "4e3a952"
old = lambda p: subprocess.run(["git", "show", f"{BASE}:{p}"], capture_output=True, text=True, check=True).stdout
new = lambda p: open(p).read()
wrong = 0


def told(what, same, more=""):
    global wrong
    wrong += not same
    print(f"{'identical' if same else 'DIFFERENT'}: {what}{more}")


def block(text, start):
    """From `start` to the end of its braces."""
    at = text.index(start)
    assert text.count(start) == 1, (start, text.count(start))
    depth, i = 0, text.index("{", at)
    while True:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
        if depth == 0:
            return text[at:i]


def same_block(path, start, allow=()):
    a, b = block(old(path), start), block(new(path), start)
    for was, now in allow:
        assert b.count(now) == 1 and a.count(was) == 1, (was, now)
        a = a.replace(was, now)
    told(f"{path}: {start.strip()} ({b.count(chr(10)) + 1} lines)", a == b, "".join(f"; but '{w.strip()}' is '{n.strip()}'" for w, n in allow))


for p in ("src/d3d11/d3d11_query.hpp", "src/d3d11/d3d11_query.cpp"):
    told(p, old(p) == new(p))
same_block("src/d3d11/d3d11_context_imm.cpp", "class MTLD3D11ImmediateContext : public ImmediateContextBase {",
           [("private:\n  std::vector<Com<MTLD3D11OcclusionQuery>>", "protected: // (for the two classes below)\n  std::vector<Com<MTLD3D11OcclusionQuery>>")])
same_block("src/d3d11/d3d11_context_imm.cpp", "ImmediateContextBase::EmitST(cmd &&fn) {")
same_block("src/d3d11/d3d11_context_imm.cpp", "ImmediateContextBase::EmitOP(cmd &&fn) {")
same_block("src/d3d11/d3d11_context_def.cpp", "class MTLD3D11DeferredContext : public DeferredContextBase {",
           [("private:\n  DeferredContextInternalState", "protected:\n  DeferredContextInternalState")])
for f in ("void d3d11_device_mutex::lock() {", "void d3d11_device_mutex::unlock() noexcept {", "bool d3d11_device_mutex::set_protected(bool Protected) {"):
    same_block("src/d3d11/d3d11_multithread.cpp", f)
for f in ("CommandQueue::CommitCurrentChunk() {", "CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {", "CommandQueue::EncodingThread() {",
          "CommandQueue::WaitForFinishThread() {"):
    same_block("src/dxmt/dxmt_command_queue.cpp", f)
for f in ("ResourceInitializer::flushInternal() {", "ResourceInitializer::flushToWait() {"):
    same_block("src/dxmt/dxmt_resource_initializer.cpp", f)

# In these nothing was changed or taken out, only functions were added that a rescue calls.
for p in ("src/d3d11/d3d11_multithread.cpp", "src/d3d11/d3d11_multithread.hpp", "src/dxmt/dxmt_command_queue.cpp", "src/dxmt/dxmt_command_queue.hpp",
          "src/dxmt/dxmt_resource_initializer.cpp"):
    a, b = old(p).splitlines(), iter(new(p).splitlines())
    told(f"{p}: every line of it, in order", all(line in b for line in a))

# The base class of both contexts: the same once what the timeline adds is taken out of it.
p = "src/d3d11/d3d11_context_impl.cpp"
text = new(p)
for was, now in ((r"InvalidateCurrentPass\(true, OTL_WHY_\w+\)", "InvalidateCurrentPass(true)"), (r"InvalidateCurrentPass\(false, OTL_WHY_\w+\)", "InvalidateCurrentPass()"),
                 (r"PromoteFlush\(OTL_WHY_\w+\)", "promote_flush = true"),
                 (r"InvalidateCurrentPass\(false, otl_hazard \? OTL_WHY_HAZARD : OTL_WHY_TARGETS\)", "InvalidateCurrentPass()"),
                 (r"InvalidateCurrentPass\(\n +(BlitKind != CommandBufferState::BlitEncoderActive),\n(.*\n){3} +\);", r"InvalidateCurrentPass(\1);"),
                 (r"bool defer_commit = false, uint32_t why = OTL_WHY_OTHER\)", "bool defer_commit = false)"),
                 (r'#include "dxmt_otl.h"\n', ""), (r"    bool otl_hazard = false;.*\n", ""), (r"otl_hazard \|=", "should_invalidate_pass |="),
                 (r"    should_invalidate_pass \|= otl_hazard;\n", ""), (r"  // The timeline's \(dxmt_otl\.h\)(.*\n)+?  virtual void otlPassEnded.*\n\n", ""),
                 (r"  /\* The pass ends no later(.*\n)+?  }\n\n", ""), (r"    if \(otl_active\(\)\) {\n(.*\n)+?    }\n(?=    switch \(cmdbuf_state)", ""),
                 (r"      if \(otl_active\(\)\) {\n(.*\n)+?      }\n\n(?=      auto allocated_encoder_argbuf_size)", ""),
                 (r"    event_markers\.clear\(\);\n", ""), (r".*event_markers; // .*\n", "")):
    text, n = re.subn(was, now, text)
    assert n, f"{p}: {was} is not there"
told(f"{p}, the timeline's arguments and records taken out", text == old(p))
if text != old(p):
    import difflib
    sys.stdout.writelines(difflib.unified_diff(old(p).splitlines(True), text.splitlines(True), BASE, "here", n=1))
print("FAILED" if wrong else "all the same")
sys.exit(wrong != 0)
