import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAddr extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String a : args) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function fn = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (fn == null) { println("NOFUNC " + a); continue; }
            DecompileResults r = di.decompileFunction(fn, 30, monitor);
            println("==== " + a + " ====");
            println(r.getDecompiledFunction() == null ? "FAIL" : r.getDecompiledFunction().getC());
        }
        di.dispose();
    }
}
