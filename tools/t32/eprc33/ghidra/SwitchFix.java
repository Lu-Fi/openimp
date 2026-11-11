// SwitchFix.java args: <file with "jraddr target..." hex lines> <offset hex>
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.pcode.JumpTable;
import java.io.*;
import java.util.*;
public class SwitchFix extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long off = Long.parseLong(a[1], 16);
        int n = 0;
        for (String line : java.nio.file.Files.readAllLines(new File(a[0]).toPath())) {
            String[] t = line.trim().split("\\s+");
            if (t.length < 2) continue;
            Address jr = toAddr(Long.parseLong(t[0], 16) + off);
            ArrayList<Address> dl = new ArrayList<>();
            for (int i = 1; i < t.length; i++) {
                Address d = toAddr(Long.parseLong(t[i], 16) + off);
                if (!dl.contains(d)) dl.add(d);
                disassemble(d);
            }
            Instruction ins = getInstructionAt(jr);
            Function f = getFunctionContaining(jr);
            if (ins == null || f == null) { println("SwitchFix: no ins/func at " + jr); continue; }
            for (Address d : dl) ins.addMnemonicReference(d, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED);
            new JumpTable(jr, dl, true, 0).writeOverride(f);
            CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            n++;
        }
        println("SwitchFix: applied " + n);
    }
}
