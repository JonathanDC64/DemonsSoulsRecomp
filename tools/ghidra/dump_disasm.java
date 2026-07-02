// dump_disasm.java -- disassemble an address range (addr, bytes, mnemonic).
//   analyzeHeadless <proj> <name> -process GAME.EXE -noanalysis \
//       -scriptPath tools/ghidra -postScript dump_disasm.java 0x80030218 0x80030250
// Writes ghidra_disasm.txt in the working directory.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.*;

public class dump_disasm extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("usage: dump_disasm.java <start> <end>"); return; }
        long start = Long.decode(args[0]), end = Long.decode(args[1]);
        PrintWriter out = new PrintWriter(new FileWriter("ghidra_disasm.txt"));
        Listing lst = currentProgram.getListing();
        out.println(String.format("==== disasm 0x%08X .. 0x%08X ====", start, end));
        Address a = toAddr(start), stop = toAddr(end);
        while (a.compareTo(stop) < 0) {
            Instruction ins = lst.getInstructionAt(a);
            if (ins == null) { a = a.add(4); continue; }
            byte[] b = ins.getBytes();
            StringBuilder bs = new StringBuilder();
            for (byte x : b) bs.append(String.format("%02x", x & 0xff));
            out.println(String.format("  %s: %-12s %s", a, bs.toString(), ins.toString()));
            a = a.add(ins.getLength());
        }
        out.close();
        println("dump_disasm done -> ghidra_disasm.txt");
    }
}
