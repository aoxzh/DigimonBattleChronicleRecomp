# Ghidra headless post-script: dump functions, call edges, strings to JSON.
# @category Analysis
import json
import os

prog = currentProgram
fm = prog.getFunctionManager()
funcs = []
edges = []
for f in fm.getFunctions(True):
    body = f.getBody()
    callees = []
    try:
        for cf in f.getCalledFunctions(None):
            if cf.getEntryPoint():
                callees.append(str(cf.getEntryPoint()))
    except Exception:
        pass
    funcs.append({
        "name": f.getName(),
        "entry": str(f.getEntryPoint()),
        "size": int(body.getNumAddresses()),
    })
    for c in callees:
        edges.append([str(f.getEntryPoint()), c])

strings = []
mem = prog.getMemory()
try:
    from ghidra.program.model.data import StringDataType
    listing = prog.getListing()
    it = listing.getDefinedData(True)
    for d in it:
        try:
            if d.hasStringValue():
                strings.append({"addr": str(d.getAddress()), "s": str(d.getValue())[:200]})
        except Exception:
            pass
except Exception:
    pass

out = {
    "entry": str(prog.getSymbolTable().getExternalEntryPointIterator().next()),
    "n_functions": len(funcs),
    "functions": funcs,
    "call_edges": edges,
    "n_strings": len(strings),
    "strings": strings,
}
path = os.path.join(os.environ.get("GHIDRA_DUMP_DIR", "."), "ghidra_symbols.json")
with open(path, "w") as fp:
    json.dump(out, fp)
print("DUMPED %d funcs, %d edges, %d strings -> %s" % (len(funcs), len(edges), len(strings), path))
