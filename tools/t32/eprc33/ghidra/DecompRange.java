// DecompRange.java args: <out> <lo hex> <hi hex>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DecompRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long lo = Long.parseLong(a[1], 16), hi = Long.parseLong(a[2], 16);
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(lo), true)) {
                if (f.getEntryPoint().getOffset() >= hi) break;
                w.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                StringBuilder cl = new StringBuilder();
                for (Function c : f.getCallingFunctions(monitor)) cl.append(c.getName()).append(' ');
                w.println("// callers: " + cl);
                StringBuilder ce = new StringBuilder();
                for (Function c : f.getCalledFunctions(monitor)) ce.append(c.getName()).append(' ');
                w.println("// callees: " + ce);
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
        di.dispose();
    }
}
