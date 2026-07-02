// dump_many.java -- decompile the functions containing each given address, in ONE
// Ghidra session. Writes ghidra_many.txt in the working dir. If Ghidra's analysis
// never disassembled/created a function at the address (only reached via indirect
// calls), force disassemble(addr)+createFunction(addr) first.
//   analyzeHeadless <proj> DeS -process EBOOT.ELF -noanalysis \
//       -scriptPath tools/ghidra -postScript dump_many.java 0x00C278C8 0x009D9220 ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;

public class dump_many extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter("ghidra_many.txt"));
        FunctionManager fm = currentProgram.getFunctionManager();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (String as : args) {
            long a;
            try { a = Long.decode(as); } catch (Exception e) { out.println("// bad addr " + as); continue; }
            Address addr = toAddr(a);
            out.println("\n//======================= " + as + " =======================");
            Function f = fm.getFunctionAt(addr);
            if (f == null) f = fm.getFunctionContaining(addr);
            if (f == null) {
                // Not reached by auto-analysis. Force it.
                try {
                    if (getInstructionAt(addr) == null) {
                        disassemble(addr);
                    }
                    f = createFunction(addr, "FORCE_" + as);
                    if (f == null) f = fm.getFunctionContaining(addr);
                    out.println("// [forced disassemble+create]");
                } catch (Exception e) {
                    out.println("// force-create failed: " + e.getMessage());
                }
            }
            if (f == null) { out.println("// no function at " + as); continue; }
            out.println("// " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults r = dec.decompileFunction(f, 120, new ConsoleTaskMonitor());
            if (r != null && r.getDecompiledFunction() != null)
                out.println(r.getDecompiledFunction().getC());
            else
                out.println("// (decompile failed: " + (r != null ? r.getErrorMessage() : "null") + ")");
        }
        out.close();
        println("dump_many done -> ghidra_many.txt");
    }
}
