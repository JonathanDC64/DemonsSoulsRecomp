// find_xref.java -- list every reference TO the given data address(es) and decompile the
// referencing functions. Useful for locating the code that reads/writes a variable.
//   analyzeHeadless <proj> <name> -process GAME.EXE -noanalysis \
//       -scriptPath tools/ghidra -postScript find_xref.java 0x801b2506 [0x...]
// Writes ghidra_xref.txt in the working directory. No hardcoded paths.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;
import java.util.*;

public class find_xref extends GhidraScript {
    PrintWriter out;
    void log(String s) { out.println(s); out.flush(); }

    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) { println("usage: find_xref.java <addr> [addr ...]"); return; }
        out = new PrintWriter(new FileWriter("ghidra_xref.txt"));
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager ref = currentProgram.getReferenceManager();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        ConsoleTaskMonitor mon = new ConsoleTaskMonitor();
        Set<Function> funcs = new LinkedHashSet<>();
        for (String a : args) {
            long t = Long.decode(a);
            log("=== refs to 0x" + Long.toHexString(t) + " ===");
            for (Reference r : ref.getReferencesTo(toAddr(t))) {
                Function cf = fm.getFunctionContaining(r.getFromAddress());
                log("  " + r.getFromAddress() + " (" + r.getReferenceType() + ") in "
                    + (cf == null ? "?" : cf.getName()));
                if (cf != null) funcs.add(cf);
            }
        }
        log("\n=== decompiled referencing functions ===");
        for (Function f : funcs) {
            try {
                DecompileResults res = dec.decompileFunction(f, 90, mon);
                if (res != null && res.getDecompiledFunction() != null) {
                    log("\n//==== " + f.getName() + " @ " + f.getEntryPoint() + " ====");
                    log(res.getDecompiledFunction().getC());
                }
            } catch (Exception e) { log("err " + e); }
        }
        out.close();
        println("find_xref done -> ghidra_xref.txt");
    }
}
