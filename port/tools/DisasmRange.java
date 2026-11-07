import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class DisasmRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        for (int k = 0; k + 1 < args.length; k += 2) {
            long a = Long.parseLong(args[k].replace("0x", ""), 16);
            int back = Integer.parseInt(args[k + 1]);
            Address start = currentProgram.getAddressFactory().getAddress(Long.toHexString(a - back * 4L));
            Address end = currentProgram.getAddressFactory().getAddress(Long.toHexString(a + 8 * 4L));
            println("---- around " + args[k]);
            InstructionIterator it = currentProgram.getListing().getInstructions(start, true);
            while (it.hasNext()) {
                Instruction in = it.next();
                if (in.getAddress().compareTo(end) > 0) break;
                println(in.getAddress() + "\t" + in.toString());
            }
        }
    }
}
