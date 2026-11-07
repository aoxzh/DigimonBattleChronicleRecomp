import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;

import java.io.PrintWriter;

public class DumpFuncs extends GhidraScript {
    private String esc(String s) {
        StringBuilder b = new StringBuilder();
        for (char c : s.toCharArray()) {
            if (c == '\t' || c == '\n' || c == '\r') b.append(' ');
            else if (c < 0x20 || c > 0x7E) b.append('?');
            else b.append(c);
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String env = System.getenv("GHIDRA_DUMP_DIR");   // where the .tsv files go (default: working directory)
        String dir = env != null && !env.isEmpty() ? (env.endsWith("/") ? env : env + "/") : "";
        PrintWriter ff = new PrintWriter(dir + "ghidra_funcs.tsv");
        PrintWriter ee = new PrintWriter(dir + "ghidra_edges.tsv");
        PrintWriter ss = new PrintWriter(dir + "ghidra_strings.tsv");
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int nf = 0, ne = 0;
        while (it.hasNext()) {
            Function fn = it.next();
            ff.println(fn.getEntryPoint() + "\t" + fn.getName() + "\t" + fn.getBody().getNumAddresses());
            nf++;
            for (Function c : fn.getCalledFunctions(null)) {
                if (c.getEntryPoint() != null) {
                    ee.println(fn.getEntryPoint() + "\t" + c.getEntryPoint());
                    ne++;
                }
            }
        }
        int ns = 0;
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            try {
                if (d.hasStringValue()) {
                    ss.println(d.getAddress() + "\t" + esc(String.valueOf(d.getValue())));
                    ns++;
                }
            } catch (Exception ex) { /* skip */ }
        }
        ff.close(); ee.close(); ss.close();
        println("DUMPED " + nf + " funcs " + ne + " edges " + ns + " strings");
    }
}
