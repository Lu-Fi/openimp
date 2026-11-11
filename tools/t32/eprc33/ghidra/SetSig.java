// SetSig.java args: <file: "hexaddr nparams [ret]"> <offset>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.*;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.util.*;
public class SetSig extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long off = Long.parseLong(a[1], 16);
        for (String line : java.nio.file.Files.readAllLines(new File(a[0]).toPath())) {
            String[] t = line.trim().split("\\s+");
            if (t.length < 2) continue;
            Function f = getFunctionAt(toAddr(Long.parseLong(t[0], 16) + off));
            int n = Integer.parseInt(t[1]);
            List<Variable> ps = new ArrayList<>();
            for (int i = 0; i < n; i++) ps.add(new ParameterImpl("param_" + (i + 1), IntegerDataType.dataType, currentProgram));
            f.replaceParameters(ps, Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.USER_DEFINED);
            if (t.length > 2 && t[2].equals("void")) f.setReturnType(VoidDataType.dataType, SourceType.USER_DEFINED);
            if (t.length > 2 && t[2].equals("int")) f.setReturnType(IntegerDataType.dataType, SourceType.USER_DEFINED);
            println("SetSig " + f.getName() + " " + f.getSignature());
        }
    }
}
