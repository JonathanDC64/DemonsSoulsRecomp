// dump_one.java -- decompile the function containing the given address.
//   analyzeHeadless <proj> <name> -process GAME.EXE -noanalysis \
//       -scriptPath tools/ghidra -postScript dump_one.java 0x8002fe1c
// Writes ghidra_one.txt in the working directory.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;

public class dump_one extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) { println("usage: dump_one.java <addr>"); return; }
        long a = Long.decode(args[0]);
        PrintWriter out = new PrintWriter(new FileWriter("ghidra_one.txt"));
        FunctionManager fm = currentProgram.getFunctionManager();
        Function f = fm.getFunctionAt(toAddr(a));
        if (f == null) f = fm.getFunctionContaining(toAddr(a));
        if (f == null) { out.println("no function at " + Long.toHexString(a)); out.close(); return; }
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        DecompileResults r = dec.decompileFunction(f, 60, new ConsoleTaskMonitor());
        out.println("//==== " + f.getName() + " @ " + f.getEntryPoint() + " ====");
        if (r != null && r.getDecompiledFunction() != null)
            out.println(r.getDecompiledFunction().getC());
        out.close();
        println("dump_one done -> ghidra_one.txt");
    }
}
