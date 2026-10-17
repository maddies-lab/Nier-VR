// Decompiles the functions at the given absolute addresses (and callees to <depth>).
// Usage (headless): ... -postScript DumpAt.java <out-dir> <depth> <address> [<address> ...]
// Each function also gets a list of its callers in <out-dir>/callers.txt.
// @category NieR-VR

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DumpAt extends GhidraScript {
    private DecompInterface decompiler;
    private File outDir;
    private final Set<Function> done = new HashSet<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        outDir = new File(args[0]);
        outDir.mkdirs();
        int depth = Integer.parseInt(args[1]);

        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        try (PrintWriter callers = new PrintWriter(new FileWriter(new File(outDir, "callers.txt"), true))) {
            for (int i = 2; i < args.length; i++) {
                Function f = getFunctionContaining(toAddr(Long.decode(args[i])));
                if (f == null) {
                    println("no function at " + args[i]);
                    continue;
                }
                callers.println("== callers of " + f.getEntryPoint());
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function c = getFunctionContaining(r.getFromAddress());
                    callers.printf("  %s from %s (%s)%n", c != null ? c.getEntryPoint() : "?", r.getFromAddress(),
                                   r.getReferenceType());
                }
                dump(f, depth);
            }
        }
        println("Dumped " + done.size() + " functions to " + outDir);
    }

    private void dump(Function f, int depth) throws Exception {
        if (f == null || depth < 0 || !done.add(f)) {
            return;
        }
        DecompileResults result = decompiler.decompileFunction(f, 60, monitor);
        try (PrintWriter out = new PrintWriter(new File(outDir, f.getEntryPoint() + ".c"))) {
            out.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
            out.println(result.decompileCompleted() ? result.getDecompiledFunction().getC() : "// decompile failed");
        }
        for (Function callee : f.getCalledFunctions(monitor)) {
            dump(callee, depth - 1);
        }
    }
}
