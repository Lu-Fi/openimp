import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.SourceType;
public class CommitParams extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long lo = Long.parseLong(a[0], 16), hi = Long.parseLong(a[1], 16);
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int pass = 0; pass < 3; pass++) {
            int n = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(lo), true)) {
                if (f.getEntryPoint().getOffset() >= hi) break;
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                if (!r.decompileCompleted()) continue;
                HighFunction hf = r.getHighFunction();
                try {
                    HighFunctionDBUtil.commitParamsToDatabase(hf, true, HighFunctionDBUtil.ReturnCommitOption.COMMIT, SourceType.ANALYSIS);
                    n++;
                } catch (Exception e) { println("CommitParams fail " + f.getName() + ": " + e); }
            }
            println("CommitParams pass " + pass + ": " + n);
        }
        di.dispose();
    }
}
