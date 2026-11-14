import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
public class NoRet extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long off = Long.parseLong(a[1], 16);
        int n = 0;
        java.util.Set<Function> fs = new java.util.HashSet<>();
        for (String line : java.nio.file.Files.readAllLines(new File(a[0]).toPath())) {
            if (line.trim().isEmpty()) continue;
            Address ad = toAddr(Long.parseLong(line.trim(), 16) + off);
            Instruction ins = getInstructionAt(ad);
            if (ins == null) continue;
            ins.setFlowOverride(FlowOverride.CALL_RETURN);
            Function f = getFunctionContaining(ad); if (f != null) fs.add(f);
            n++;
        }
        long lo = Long.parseLong(a[2], 16) + off, hi = Long.parseLong(a[3], 16) + off;
        for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(lo), true)) {
            if (f.getEntryPoint().getOffset() >= hi) break;
            CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        }
        println("NoRet: " + n);
    }
}
